// Copyright 2015 The rkt Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/vfs.h>
#include <dirent.h>
#include <inttypes.h>

#define err_out(_fmt, _args...)						\
		fprintf(stderr, "Error: " _fmt "\n", ##_args);
static int exit_err;
#define exit_if(_cond, _fmt, _args...)					\
	exit_err++;							\
	if(_cond) {							\
		err_out(_fmt, ##_args);					\
		exit(exit_err);						\
	}
#define pexit_if(_cond, _fmt, _args...)					\
	exit_if(_cond, _fmt ": %s", ##_args, strerror(errno))

#define goto_if(_cond, _lbl, _fmt, _args...)				\
	if(_cond) {							\
		err_out(_fmt, ##_args);					\
		goto _lbl;						\
	}
#define pgoto_if(_cond, _lbl, _fmt, _args...)				\
	goto_if(_cond, _lbl, _fmt ": %s", ##_args, strerror(errno));

#define nelems(_array) \
	(sizeof(_array) / sizeof(_array[0]))
#define lenof(_str) \
	(sizeof(_str) - 1)

#define MACHINE_ID_LEN		lenof("0123456789abcdef0123456789ab")
#define MACHINE_NAME_LEN	lenof("rkt-01234567-89ab-cdef-0123-456789ab")

#define UNMAPPED ((uid_t) -1)

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

typedef struct _dir_op_t {
	const char	*name;
	mode_t		mode;
} dir_op_t;

typedef struct _mount_point_t {
	const char	*source;
	const char	*target;
	const char	*type;
	const char	*options;
	unsigned long	flags;
} mount_point;

#define dir(_name, _mode) \
	{ .name = _name, .mode = _mode }

static int get_machine_name(char *out, int out_len) {
	int	fd;
	char	buf[MACHINE_ID_LEN + 1];

	pgoto_if((fd = open("/etc/machine-id", O_RDONLY)) == -1,
		_fail, "Error opening \"/etc/machine-id\"");
	pgoto_if(read(fd, buf, MACHINE_ID_LEN) == -1,
		_fail_fd, "Error reading \"/etc/machine-id\"");
	pgoto_if(close(fd) != 0,
		_fail, "Error closing \"/etc/machine-id\"");
	goto_if(snprintf(out, out_len,
			"rkt-%.8s-%.4s-%.4s-%.4s-%.8s",
			buf, buf+8, buf+12, buf+16, buf+20) >= out_len,
		_fail, "Error constructing machine name");

	return 1;

_fail_fd:
	close(fd);
_fail:
	return 0;
}

static int ensure_etc_hosts_exists(const char *root, int rootfd) {
	char	name[MACHINE_NAME_LEN + 1];
	char	hosts[128];
	int	fd, len;

	if(faccessat(rootfd, "etc/hosts", F_OK, AT_EACCESS) == 0)
		return 1;

	goto_if(!get_machine_name(name, sizeof(name)),
		_fail, "Failed to get machine name");
	goto_if((len = snprintf(hosts, sizeof(hosts),
			"%s\t%s\t%s\t%s\n",
			"127.0.0.1", name,
			"localhost", "localhost.localdomain")) >= sizeof(hosts),
		_fail, "/etc/hosts line too long: \"%s\"", hosts);
	pgoto_if((fd = openat(rootfd, "etc/hosts", O_WRONLY|O_CREAT, 0644)) == -1,
		_fail, "Failed to create \"%s/etc/hosts\"", root);
	pgoto_if(write(fd, hosts, len) != len,
		_fail_fd, "Failed to write \"%s/etc/hosts\"", root);
	pgoto_if(close(fd) != 0,
		_fail, "Failed to close \"%s/etc/hosts\"", root);

	return 1;

_fail_fd:
	close(fd);
_fail:
	return 0;
}

static void mount_at(const char *root, const mount_point *mnt)
{
	char to[4096];
	exit_if(snprintf(to, sizeof(to), "%s/%s", root, mnt->target) >= sizeof(to),
		"Path too long: \"%s\"", to);
	pexit_if(mount(mnt->source, to, mnt->type,
		       mnt->flags, mnt->options) == -1,
		 "Mounting \"%s\" on \"%s\" failed", mnt->source, to);
}

static void mount_sys(const char *root)
{
	int i;
	char to[4096];
	struct statfs fs;
	DIR *dir = NULL;
	struct dirent *d;
	const mount_point mnt_rec = { "/sys", "sys", "bind", NULL, MS_BIND|MS_REC };
	const mount_point sys_bind_table[] = {
		{ "/sys", "sys", "bind", NULL, MS_BIND },
		{ "/sys/fs/cgroup", "sys/fs/cgroup", "bind", NULL, MS_BIND },
	};

	pexit_if(statfs("/sys/fs/cgroup", &fs) != 0,
	         "Cannot statfs /sys/fs/cgroup");
	if (fs.f_type == (typeof(fs.f_type)) CGROUP2_SUPER_MAGIC) {
		/* With the unified cgroup hierarchy, recursive bind mounts
		 * are fine. */
		mount_at(root, &mnt_rec);
		return;
	}

	// For security reasons recent Linux kernels do not allow to bind-mount non-recursively
	// if it would give read-write access to other subdirectories mounted as read-only.
	// Hence we have to check if we are in a user namespaced environment and bind mount recursively instead.
	if (access("/proc/1/uid_map", F_OK) == 0) {
		FILE *f;
		int k;
		uid_t uid_base, uid_shift, uid_range;

		pexit_if((f = fopen("/proc/1/uid_map", "re")) == NULL,
			 "Unable to open /proc/1/uid_map");

		if (sizeof(uid_t) == 4) {
			k = fscanf(f, "%"PRIu32" %"PRIu32" %"PRIu32,
				   &uid_base, &uid_shift, &uid_range);
		} else {
			k = fscanf(f, "%"PRIu16" %"PRIu16" %"PRIu16,
				   &uid_base, &uid_shift, &uid_range);
		}
		pexit_if(fclose(f) != 0, "Unable to close /proc/1/uid_map");
		pexit_if(k != 3, "Invalid uid_map format");

		// do a recursive bind mount if we are in a user namespace having a parent namespace set,
		// i.e. either one of uid base, shift, or the range is set, see user_namespaces(7).
		if (uid_base != 0 || uid_shift != 0 || uid_range != UNMAPPED) {
			mount_at(root, &mnt_rec);
			return;
		}
	}

	/* With cgroup-v1, rkt and systemd-nspawn add more cgroup
	 * bind-mounts to control which files are read-only. To avoid
	 * a quadratic progression, prepare-app does not bind mount
	 * /sys recursively. See:
	 * https://github.com/coreos/rkt/issues/2351 */
	for (i = 0; i < nelems(sys_bind_table); i++) {
		mount_at(root, &sys_bind_table[i]);
	}

	exit_if(snprintf(to, sizeof(to), "%s/%s", root, "sys/fs/cgroup") >= sizeof(to),
		"Path too long: \"%s\"", to);
	pexit_if(!(dir = opendir(to)), "Failed to open directory \"%s\"", to)
	errno = 0;
	while ((d = readdir(dir))) {
		if (d->d_type != DT_DIR)
			continue;
		if (strcmp(d->d_name, ".") == 0)
			continue;
		if (strcmp(d->d_name, "..") == 0)
			continue;

		exit_if(snprintf(to, sizeof(to), "sys/fs/cgroup/%s", d->d_name) >= sizeof(to),
			"Path too long: \"%s\"", to);

		mount_point mnt = { to, to, "bind", NULL, MS_BIND };
		mount_at(root, &mnt);
	}
	pexit_if(errno != 0, "Failed to read directory \"%s\"", to);
	pexit_if(closedir(dir) != 0, "Failed to close directory");
}

int main(int argc, char *argv[])
{
	static const char *unlink_paths[] = {
		"dev/shm",
		"dev/ptmx",
		NULL
	};
	static const dir_op_t dirs[] = {
		dir("dev",	0755),
		dir("dev/net",	0755),
		dir("dev/shm",	0755),
		dir("etc",	0755),
		dir("proc",	0755),
		dir("sys",	0755),
		dir("tmp",	01777),
		dir("dev/pts",	0755),
		dir("run",			0755),
		dir("run/systemd",		0755),
		dir("run/systemd/journal",	0755),
	};
	static const char *devnodes[] = {
		"/dev/null",
		"/dev/zero",
		"/dev/full",
		"/dev/random",
		"/dev/urandom",
		"/dev/tty",
		"/dev/net/tun",
		"/dev/console",
		NULL
	};
	static const mount_point dirs_mount_table[] = {
		{ "/proc", "/proc", "bind", NULL, MS_BIND|MS_REC },
		{ "/dev/shm", "/dev/shm", "bind", NULL, MS_BIND },
		{ "/dev/pts", "/dev/pts", "bind", NULL, MS_BIND },
		{ "/run/systemd/journal", "/run/systemd/journal", "bind", NULL, MS_BIND },
		/* /sys is handled separately */
	};
	static const mount_point files_mount_table[] = {
		{ "/etc/rkt-resolv.conf", "/etc/resolv.conf", "bind", NULL, MS_BIND },
	};
	const char *root;
	int rootfd;
	char to[4096];
	int i;

	exit_if(argc < 2,
		"Usage: %s /path/to/root", argv[0]);

	root = argv[1];

	/* Make stage2's root a mount point. Chrooting an application in a
	 * directory which is not a mount point is not nice because the
	 * application would not be able to remount "/" it as private mount.
	 * This allows Docker to run inside rkt.
	 * The recursive flag is to preserve volumes mounted previously by
	 * systemd-nspawn via "rkt run -volume".
	 * */
	pexit_if(mount(root, root, "bind", MS_BIND | MS_REC, NULL) == -1,
			"Make / a mount point failed");

	rootfd = open(root, O_DIRECTORY | O_CLOEXEC);
	pexit_if(rootfd < 0,
		"Failed to open directory \"%s\"", root);

	/* Some images have annoying symlinks that are resolved as dangling
	 * links before the chroot in stage1. E.g. "/dev/shm" -> "/run/shm"
	 * Just remove the symlinks.
         */
	for (i = 0; unlink_paths[i]; i++) {
		pexit_if(unlinkat(rootfd, unlink_paths[i], 0) != 0
			 && errno != ENOENT && errno != EISDIR,
			 "Failed to unlink \"%s\"", unlink_paths[i])
	}

	/* Create the directories */
	umask(0);
	for (i = 0; i < nelems(dirs); i++) {
		const dir_op_t *d = &dirs[i];
		pexit_if(mkdirat(rootfd, d->name, d->mode) == -1 &&
			 errno != EEXIST,
			"Failed to create directory \"%s/%s\"", root, d->name);
	}

	exit_if(!ensure_etc_hosts_exists(root, rootfd),
		"Failed to ensure \"%s/etc/hosts\" exists", root);

	close(rootfd);

	/* systemd-nspawn already creates few /dev entries in the container
	 * namespace: copy_devnodes()
	 * http://cgit.freedesktop.org/systemd/systemd/tree/src/nspawn/nspawn.c?h=v219#n1345
	 *
	 * But they are not visible by the apps because they are "protected" by
	 * the chroot.
	 *
	 * Bind mount them individually over the chroot border.
	 *
	 * Do NOT bind mount the whole directory /dev because it would shadow
	 * potential individual bind mount by stage0 ("rkt run --volume...").
	 *
	 * Do NOT use mknod, it would not work for /dev/console because it is
	 * a bind mount to a pts and pts device nodes only work when they live
	 * on a devpts filesystem.
	 */
	for (i = 0; devnodes[i]; i++) {
		const char *from = devnodes[i];
		int fd;

		/* If the file does not exist, skip it. It might be because
		 * the kernel does not provide it (e.g. kernel compiled without
		 * CONFIG_TUN) or because systemd-nspawn does not provide it
		 * (/dev/net/tun is not available with systemd-nspawn < v217
		 */
		if (access(from, F_OK) != 0)
			continue;

		exit_if(snprintf(to, sizeof(to), "%s%s", root, from) >= sizeof(to),
			"Path too long: \"%s\"", to);

		/* The mode does not matter: it will be bind-mounted over.
		 */
		fd = open(to, O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY, 0644);
		if (fd != -1)
			close(fd);

		pexit_if(mount(from, to, "bind", MS_BIND, NULL) == -1,
				"Mounting \"%s\" on \"%s\" failed", from, to);
	}

	/* Bind mount directories */
	for (i = 0; i < nelems(dirs_mount_table); i++) {
		mount_at(root, &dirs_mount_table[i]);
	}

	/* Bind mount /sys: handled differently, depending on cgroups */
	mount_sys(root);

	/* Bind mount files, if the source exists */
	for (i = 0; i < nelems(files_mount_table); i++) {
		const mount_point *mnt = &files_mount_table[i];
		int fd;

		exit_if(snprintf(to, sizeof(to), "%s/%s", root, mnt->target) >= sizeof(to),
			"Path too long: \"%s\"", to);
		if (access(mnt->source, F_OK) != 0)
			continue;
		if (access(to, F_OK) != 0) {
			pexit_if((fd = creat(to, 0644)) == -1,
				"Cannot create file: \"%s\"", to);
			pexit_if(close(fd) == -1,
				"Cannot close file: \"%s\"", to);
		}
		pexit_if(mount(mnt->source, to, mnt->type,
			       mnt->flags, mnt->options) == -1,
				"Mounting \"%s\" on \"%s\" failed", mnt->source, to);
	}

	/* /dev/ptmx -> /dev/pts/ptmx */
	exit_if(snprintf(to, sizeof(to), "%s/dev/ptmx", root) >= sizeof(to),
		"Path too long: \"%s\"", to);
	pexit_if(symlink("/dev/pts/ptmx", to) == -1 && errno != EEXIST,
		"Failed to create /dev/ptmx symlink");

	/* /dev/log -> /run/systemd/journal/dev-log */
	exit_if(snprintf(to, sizeof(to), "%s/dev/log", root) >= sizeof(to),
		"Path too long: \"%s\"", to);
	pexit_if(symlink("/run/systemd/journal/dev-log", to) == -1 && errno != EEXIST,
		"Failed to create /dev/log symlink");

	return EXIT_SUCCESS;
}
