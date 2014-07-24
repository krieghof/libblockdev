/*
 * Copyright (C) 2014  Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Vratislav Podzimek <vpodzime@redhat.com>
 */

#include <glib.h>
#include <unistd.h>
#include <exec.h>
#include <libdevmapper.h>
#include <dmraid/dmraid.h>
#include <libudev.h>

#include "dm.h"

/**
 * SECTION: dm
 * @short_description: libblockdev plugin for basic operations with device mapper
 * @title: DeviceMapper
 * @include: dm.h
 *
 * A libblockdev plugin for basic operations with device mapper.
 */

/**
 * bd_dm_create_linear:
 * @map_name: name of the map
 * @device: device to create map for
 * @length: length of the mapping in sectors
 * @uuid: (allow-none): UUID for the new dev mapper device or %NULL if not specified
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: whether the new linear mapping @map_name was successfully created
 * for the @device or not
 */
gboolean bd_dm_create_linear (gchar *map_name, gchar *device, guint64 length, gchar *uuid, gchar **error_message) {
    gboolean success = FALSE;
    gchar *argv[9] = {"dmsetup", "create", map_name, "--table", NULL, NULL, NULL, NULL, NULL};

    gchar *table = g_strdup_printf ("0 %"G_GUINT64_FORMAT" linear %s 0", length, device);
    argv[4] = table;

    if (uuid) {
        argv[5] = "-u";
        argv[6] = uuid;
        argv[7] = device;
    } else
        argv[5] = device;

    success = bd_utils_exec_and_report_error (argv, error_message);
    g_free (table);

    return success;
}

/**
 * bd_dm_remove:
 * @map_name: name of the map to remove
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: whether the @map_name map was successfully removed or not
 */
gboolean bd_dm_remove (gchar *map_name, gchar **error_message) {
    gchar *argv[4] = {"dmsetup", "remove", map_name, NULL};

    return bd_utils_exec_and_report_error (argv, error_message);
}

/**
 * bd_dm_name_from_node:
 * @dm_node: name of the DM node (e.g. "dm-0")
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: map name of the map providing the @dm_node device or %NULL
 * (@error_message contains the error in such cases)
 */
gchar* bd_dm_name_from_node (gchar *dm_node, gchar **error_message) {
    gchar *ret = NULL;
    gboolean success = FALSE;
    GError *error;

    gchar *sys_path = g_strdup_printf ("/sys/class/block/%s/dm/name", dm_node);

    if (access (sys_path, R_OK) != 0) {
        g_free (sys_path);
        *error_message = g_strdup ("Failed to access dm node's parameters under /sys");
        return NULL;
    }

    success = g_file_get_contents (sys_path, &ret, NULL, &error);
    g_free (sys_path);

    if (!success) {
        *error_message = g_strdup (error->message);
         g_clear_error (&error);
        return NULL;
    }

    return g_strstrip (ret);
}

/**
 * bd_dm_node_from_name:
 * @map_name: name of the queried DM map
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: DM node name for the @map_name map or %NULL (@error_message contains
 * the error in such cases)
 */
gchar* bd_dm_node_from_name (gchar *map_name, gchar **error_message) {
    GError *error = NULL;
    gchar *symlink = NULL;
    gchar *ret = NULL;
    gchar *dev_mapper_path = g_strdup_printf ("/dev/mapper/%s", map_name);

    symlink = g_file_read_link (dev_mapper_path, &error);
    if (!symlink) {
        *error_message = g_strdup (error->message);
        g_error_free(error);
        g_free (dev_mapper_path);
        return FALSE;
    }

    g_strstrip (symlink);
    ret = g_path_get_basename (symlink);

    g_free (symlink);
    g_free (dev_mapper_path);

    return ret;
}

/**
 * bd_dm_map_exists:
 * @map_name: name of the queried map
 * @live_only: whether to go through the live maps only or not
 * @active_only: whether to ignore suspended maps or not
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: whether the given @map_name exists (and is live if @live_only is
 * %TRUE (and is active if @active_only is %TRUE)). If %FALSE is returned,
 * @error_message indicates whether error appeared (non-%NULL) or not (%NULL).
 */
gboolean bd_dm_map_exists (gchar *map_name, gboolean live_only, gboolean active_only, gchar **error_message) {
    struct dm_task *task_list = NULL;
    struct dm_task *task_info = NULL;
	struct dm_names *names = NULL;
    struct dm_info info;
	guint64 next = 0;
    gboolean ret = FALSE;

    if (geteuid () != 0) {
        *error_message = g_strdup ("Not running as root, cannot query DM maps");
        return FALSE;
    }

    /* TODO: init DM logging here to throw discard errors? (they will appear on
       stderr by default) */

    task_list = dm_task_create(DM_DEVICE_LIST);
	if (!task_list) {
        g_warning ("Failed to create DM task");
        *error_message = g_strdup ("Failed to create DM task");
        return FALSE;
    }

    dm_task_run(task_list);
	names = dm_task_get_names(task_list);

    if (!names || !names->dev)
        return FALSE;

    do {
        names = (void *)names + next;
        next = names->next;
        /* we are searching for the particular map_name map */
        if (g_strcmp0 (map_name, names->name) != 0)
            /* not matching, skip */
            continue;

        /* get device info */
        task_info = dm_task_create(DM_DEVICE_INFO);
        if (!task_info) {
            g_warning ("Failed to create DM task");
            *error_message = g_strdup ("Failed to create DM task");
            break;
        }

        dm_task_set_name(task_info, names->name);
        dm_task_run(task_info);
        dm_task_get_info(task_info, &info);

        if (!info.exists)
            /* doesn't exist, try next one */
            continue;

        /* found existing name match, let's test the restrictions */
        ret = TRUE;
        if (live_only)
            ret = info.live_table;
        if (active_only)
            ret = ret && !info.suspended;

        dm_task_destroy (task_info);
        if (ret)
            /* found match according to restrictions */
            break;
    } while (next);

    dm_task_destroy (task_list);

    return ret;
}

/**
 * raid_dev_matches_spec: (skip)
 *
 * Returns: whether the device specified by @sysname matches the spec given by @name,
 *          @uuid, @major and @minor
 */
static gboolean raid_dev_matches_spec (struct raid_dev *raid_dev, gchar *name, gchar *uuid, gint major, gint minor) {
    gchar const *dev_name = NULL;
    gchar const *dev_uuid;
    gchar const *major_str;
    gchar const *minor_str;
    struct udev *context;
    struct udev_device *device;
    gboolean ret = TRUE;

    /* find the second '/' to get name (the rest of the string) */
    dev_name = strchr (raid_dev->di->path, '/');
    if (dev_name && strlen (dev_name) > 1) {
        dev_name++;
        dev_name = strchr (dev_name, '/');
    }
    if (dev_name && strlen (dev_name) > 1) {
        dev_name++;
    }
    else
        dev_name = NULL;

    /* if we don't have the name, we cannot check any match */
    g_return_val_if_fail (dev_name, FALSE);

    if (name && strcmp (dev_name, name) != 0) {
        return FALSE;
    }

    context = udev_new ();
    device = udev_device_new_from_subsystem_sysname (context, "block", dev_name);
    dev_uuid = udev_device_get_property_value (device, "UUID");
    major_str = udev_device_get_property_value (device, "MAJOR");
    minor_str = udev_device_get_property_value (device, "MINOR");

    if (uuid && (g_strcmp0 (uuid, "") != 0) && (g_strcmp0 (uuid, dev_uuid) != 0))
        ret = FALSE;

    if (major >= 0 && (atoi (major_str) != major))
        ret = FALSE;

    if (minor >= 0 && (atoi (minor_str) != minor))
        ret = FALSE;

    udev_device_unref (device);
    udev_unref (context);

    return ret;
}

/**
 * process_raid_set: (skip)
 */
static void find_dev_in_raid_set (gchar *name, gchar *uuid, gint major, gint minor, struct lib_context *lc, struct raid_set *rs, GPtrArray *ret_sets) {
    struct raid_set *subset;
    struct raid_dev *dev;

    if (T_GROUP(rs) || !list_empty(&(rs->sets))) {
        for_each_subset (rs, subset)
            find_dev_in_raid_set (name, uuid, major, minor, lc, subset, ret_sets);
    } else {
        for_each_device (rs, dev) {
            if (raid_dev_matches_spec (dev, name, uuid, major, minor))
                g_ptr_array_add (ret_sets, g_strdup (rs->name));
        }
    }
}

/**
 * bd_dm_get_member_raid_sets:
 * @name: (allow-none): name of the member
 * @uuid: (allow-none): uuid of the member
 * @major: major number of the device or -1 if not specified
 * @minor: minor number of the device or -1 if not specified
 * @error_message: (out): variable to store error message to (if any)
 *
 * Returns: (transfer full) (array zero-terminated=1): list of names of the RAID sets related to
 * the member or %NULL in case of error
 *
 * One of @name, @uuid or @major:@minor has to be given.
 */
gchar** bd_dm_get_member_raid_sets (gchar *name, gchar *uuid, gint major, gint minor, gchar **error_message) {
    guint64 i = 0;
    gint rc = 0;
    gchar *argv[] = {"blockdev.dmraid", NULL};
    struct lib_context *lc;
    struct raid_set *rs;
    GPtrArray *ret_sets = g_ptr_array_new ();
    gchar **ret = NULL;

    /* the code for this function was cherry-picked from the pyblock code */

    /* initialize dmraid library context */
    lc = libdmraid_init (1, (gchar **)argv);

    rc = discover_devices (lc, NULL);
    if (!rc) {
        *error_message = g_strdup ("Failed to discover devices");
        libdmraid_exit (lc);
        return NULL;
    }
    discover_raid_devices (lc, NULL);

    if (!count_devices (lc, RAID)) {
        *error_message = g_strdup ("No RAIDs discovered");
        libdmraid_exit (lc);
        return NULL;
    }

    argv[0] = NULL;
    if (!group_set (lc, argv)) {
        *error_message = g_strdup ("Failed to group_set");
        libdmraid_exit (lc);
        return NULL;
    }

    for_each_raidset (lc, rs) {
        find_dev_in_raid_set (name, uuid, major, minor, lc, rs, ret_sets);
    }

    /* now create the return value -- NULL-terminated array of strings */
    ret = g_new (gchar*, ret_sets->len + 1);
    for (i=0; i < ret_sets->len; i++)
        ret[i] = (gchar*) g_ptr_array_index (ret_sets, i);
    ret[i] = NULL;

    g_ptr_array_free (ret_sets, FALSE);

    libdmraid_exit (lc);
    return ret;
}
