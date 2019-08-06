/*
 * Copyright © 2012 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Peter Hutterer (peter.hutterer@redhat.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "libwacom.h"
#include <unistd.h>

static WacomDeviceDatabase *db_old;
static WacomDeviceDatabase *db_new;

static int
scandir_filter(const struct dirent *entry)
{
	return strncmp(entry->d_name, ".", 1);
}

static void
rmtmpdir(const char *dir)
{
	int nfiles;
	g_autofree struct dirent **files;

	nfiles = scandir(dir, &files, scandir_filter, alphasort);
	while(nfiles--)
	{
		g_autofree char *path = NULL;
		g_assert(asprintf(&path, "%s/%s", dir, files[nfiles]->d_name) != -1);
		g_assert(path);
		g_assert(remove(path) != -1);
		free(files[nfiles]);
		path = NULL;
	}

	g_assert(remove(dir) != -1);
}

static void
find_matching(gconstpointer data)
{
	g_autofree WacomDevice **devs_old = NULL,
			       **devs_new = NULL;
	WacomDevice **devices, **d;
	WacomDevice *other;
	gboolean found = FALSE;
	int index = GPOINTER_TO_INT(data);

	devs_old = libwacom_list_devices_from_database(db_old, NULL);
	devs_new = libwacom_list_devices_from_database(db_new, NULL);

	/* Make sure each device in old has a device in new */
	devices = devs_old;
	other = devs_new[index];
	for (d = devices; *d; d++) {
		if (libwacom_compare(other, *d, WCOMPARE_MATCHES) == 0) {
			found = TRUE;
			break;
		}
	}
	g_assert_true(found);

	/* Make sure each device in new has a device in old */
	devices = devs_new;
	other = devs_old[index];
	found = FALSE;
	for (d = devices; *d; d++) {
		/* devices with multiple matches will have multiple
		 * devices in the list */
		if (libwacom_compare(other, *d, WCOMPARE_MATCHES) == 0) {
			found = TRUE;
			break;
		}
	}
	g_assert_true(found);
}

static void
test_database_size(void)
{
	int sz1, sz2;
	g_autofree WacomDevice **d1 = NULL, **d2 = NULL;

	d1 = libwacom_list_devices_from_database(db_old, NULL);
	d2 = libwacom_list_devices_from_database(db_new, NULL);
	g_assert_nonnull(d1);
	g_assert_nonnull(d2);

	sz1 = 0;
	for (WacomDevice **d = d1; *d; d++)
		sz1++;
	sz2 = 0;
	for (WacomDevice **d = d2; *d; d++)
		sz2++;
	g_assert_cmpint(sz1, ==, sz2);
}

static int
compare_databases(WacomDeviceDatabase *orig, WacomDeviceDatabase *new)
{
	int i;
	g_autofree WacomDevice **devs_new;
	WacomDevice **n;

	g_test_add_func("/dbverify/database-sizes", test_database_size);

	devs_new = libwacom_list_devices_from_database(new, NULL);

	for (n = devs_new, i = 0 ; *n; n++, i++)
	{
		char buf[1024];

		/* We need to add the test index to avoid duplicate
		   test names */
		snprintf(buf, sizeof(buf), "/dbverify/%03d/%04x:%04x-%s",
			 i,
			 libwacom_get_vendor_id(*n),
			 libwacom_get_product_id(*n),
			 libwacom_get_name(*n));
		g_test_add_data_func(buf, GINT_TO_POINTER(i), find_matching);
	}

	return g_test_run();
}

/* write out the current db, read it back in, compare */
static void
duplicate_database(WacomDeviceDatabase *db, const char *dirname)
{
	g_autofree WacomDevice **devices = NULL;
	WacomDevice **device;
	int i;

	devices = libwacom_list_devices_from_database(db, NULL);
	g_assert(devices);
	g_assert(*devices);

	for (device = devices, i = 0; *device; device++, i++) {
		int i;
		int fd;
		g_autofree char *path = NULL;
		int nstyli;
		const int *styli;

		g_assert(asprintf(&path, "%s/%s.tablet", dirname,
				libwacom_get_match(*device)) != -1);
		g_assert(path);
		fd = open(path, O_WRONLY|O_CREAT, S_IRWXU);
		g_assert(fd >= 0);
		libwacom_print_device_description(fd, *device);
		close(fd);

		if (!libwacom_has_stylus(*device))
			continue;

		styli = libwacom_get_supported_styli(*device, &nstyli);
		for (i = 0; i < nstyli; i++) {
			g_autofree char *path = NULL;
			int fd_stylus;
			const WacomStylus *stylus;

			g_assert(asprintf(&path, "%s/%#x.stylus", dirname, styli[i]) != -1);
			stylus = libwacom_stylus_get_for_id(db, styli[i]);
			g_assert(stylus);
			fd_stylus = open(path, O_WRONLY|O_CREAT, S_IRWXU);
			g_assert(fd_stylus >= 0);
			libwacom_print_stylus_description(fd_stylus, stylus);
			close(fd_stylus);
		}
	}
}

static WacomDeviceDatabase *
load_database(void)
{
	WacomDeviceDatabase *db;
	const char *datadir;

	datadir = getenv("LIBWACOM_DATA_DIR");
	if (!datadir)
		datadir = TOPSRCDIR"/data";

	db = libwacom_database_new_for_path(datadir);
	if (!db)
		printf("Failed to load data from %s", datadir);

	g_assert(db);
	return db;
}

int main(int argc, char **argv)
{
	int rc = 1;
	WacomDeviceDatabase *db = NULL;
	g_autofree char *dirname = g_strdup("tmp.dbverify.XXXXXX");

	g_test_init(&argc, &argv, NULL);
	g_test_set_nonfatal_assertions();

	db = load_database();

	g_assert(mkdtemp(dirname)); /* just check for non-null to avoid
				       Coverity complaints */

	duplicate_database(db, dirname);
	db_new = libwacom_database_new_for_path(dirname);
	g_assert(db_new);

	db_old = db;
	rc = compare_databases(db_old, db_new);
	libwacom_database_destroy(db_new);
	libwacom_database_destroy(db_old);

	rmtmpdir(dirname);

	return rc;
}

/* vim: set noexpandtab tabstop=8 shiftwidth=8: */
