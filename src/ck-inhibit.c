/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2014 Eric Koegel <eric.koegel@gmail.com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#include <glib.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include "ck-inhibit.h"

#define CK_INHIBIT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CK_TYPE_INHIBIT, CkInhibitPrivate))

struct CkInhibitPrivate
{
        /*
         * Who is a human-readable, descriptive string of who is taking
         * the lock. Example: "Xfburn"
         * We use this as unique identifier for the lock since an app
         * isn't supposed to have multiple locks.
         */
        const gchar *who;
        /*
         * What is a colon-separated list of lock types.
         * The list of lock types are: shutdown, sleep, idle,
         * handle-power-key, handle-suspend-key, handle-hibernate-key.
         * Example: "shutdown:idle"
         */
        const gchar *what;
        /*
         * Why is a human-readable, descriptive string of why the program
         * is taking the lock. Example: "Burning a DVD, interrupting now
         * will ruin the DVD."
         */
        const gchar *why;
        /*
         * named_pipe is a named pipe that the user app will hold onto
         * while they want the lock to be held. When they close all
         * references to the named pipe then the lock is released and
         * this object can be destroyed.
         */
        gint         named_pipe;
        /* named_pipe_path is the location the named pipe is created from */
        gchar *named_pipe_path;
        /* fd_source is the event source id for the g_unix_fd_add call */
        gint fd_source;
};

static void     ck_inhibit_class_init  (CkInhibitClass *klass);
static void     ck_inhibit_init        (CkInhibit      *inhibit);
static void     ck_inhibit_finalize    (GObject        *object);

G_DEFINE_TYPE (CkInhibit, ck_inhibit, G_TYPE_OBJECT)


static void
ck_inhibit_class_init (CkInhibitClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = ck_inhibit_finalize;

        g_type_class_add_private (klass, sizeof (CkInhibitPrivate));
}

static void
ck_inhibit_init (CkInhibit *inhibit)
{
        inhibit->priv = CK_INHIBIT_GET_PRIVATE (inhibit);

        inhibit->priv->named_pipe = -1;
}

static void
ck_inhibit_finalize (GObject *object)
{
        CkInhibit *inhibit;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CK_IS_INHIBIT (object));

        inhibit = CK_INHIBIT (object);

        g_return_if_fail (inhibit->priv != NULL);

        G_OBJECT_CLASS (ck_inhibit_parent_class)->finalize (object);
}

/*
 * Creates the /run/ConsoleKit/inhibit directory. The
 * state_files will exist inside this directory.
 * Returns TRUE on success.
 */
static gboolean
create_inhibit_base_directory (void)
{
        gint res;

        errno = 0;
        res = g_mkdir_with_parents (LOCALSTATEDIR "/run/ConsoleKit/inhibit",
                                    S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        if (res < 0) {
                g_warning ("Unable to create directory %s (%s)",
                           LOCALSTATEDIR "/run/ConsoleKit/inhibit",
                           g_strerror (errno));

                return FALSE;
        }

        if (g_chmod (LOCALSTATEDIR "/run/ConsoleKit/inhibit", 0755) == -1) {
                g_warning ("Failed to change permissions for %s",
                           LOCALSTATEDIR "/run/ConsoleKit/inhibit");
        }

        return TRUE;
}

static gboolean
cb_named_pipe_close (gint fd,
                     GIOCondition condition,
                     gpointer user_data)
{
        g_warning ("cb_named_pipe_close");
        return FALSE;
}

/*
 * Creates the named pipe.
 * Returns the fd if successful (return >= 0) or -1
 */
static gint
create_named_pipe (CkInhibit *inhibit)
{
        CkInhibitPrivate *priv;

        g_return_val_if_fail (CK_IS_INHIBIT (inhibit), -1);

        priv = CK_INHIBIT_GET_PRIVATE (inhibit);

        /* Basic error checking */
        if (priv->named_pipe != -1) {
                g_warning ("Attempting to create an inhibit fd when one already exists");
                return -1;
        }

        if (priv->named_pipe_path == NULL) {
                g_warning ("named_pipe_path cannot be NULL");
                return -1;
        }

        /* create the named pipe */
        errno = 0;
        if (mknod (priv->named_pipe_path, S_IFIFO | 0600 , 0) == -1) {
                g_warning ("failed to create named pipe: %s",
                           g_strerror(errno));
                return -1;
        }

        /* open our side */
        priv->named_pipe = g_open (priv->named_pipe_path, O_RDONLY|O_CLOEXEC|O_NDELAY);
        if (priv->named_pipe < 0) {
                g_warning ("failed to open the named pipe for reading %s",
                           g_strerror(errno));
                return -1;
        }

        /* Monitor the named pipe */
        priv->fd_source = g_unix_fd_add (priv->named_pipe,
                                         G_IO_HUP,
                                         (GUnixFDSourceFunc)cb_named_pipe_close,
                                         inhibit);

        /* open the client side of the named pipe and return it */
        return open(priv->named_pipe_path, O_WRONLY|O_CLOEXEC|O_NDELAY);
}

/*
 * Initializes the lock fd and populates the inhibit object with data.
 * Returns the named pipe (a file descriptor) on success. This is a value
 * of 0 or greater.
 * Returns a CkInhbitError on failure.
 */
gint
ck_create_inhibit_lock (CkInhibit   *inhibit,
                        const gchar *who,
                        const gchar *what,
                        const gchar *why)
{
        CkInhibitPrivate *priv;

        g_return_val_if_fail (CK_IS_INHIBIT (inhibit), CK_INHIBIT_ERROR_INVALID_INPUT);

        /* These fields only get set here and are mandatory */
        if (!who || !what || !why) {
                g_warning ("who, what, and why and mandatory for inhibit locks");
                return CK_INHIBIT_ERROR_INVALID_INPUT;
        }

        priv = CK_INHIBIT_GET_PRIVATE (inhibit);

        priv->who = who;
        priv->what = what;
        priv->why = why;
        priv->named_pipe_path = g_strdup_printf (LOCALSTATEDIR "/run/ConsoleKit/inhibit/%s", who);

        if(priv->named_pipe_path == NULL) {
                g_warning ("Failed to allocate memory for inhibit state_file string");
                return CK_INHIBIT_ERROR_OOM;
        }

        /* always make sure we have a directory to work in */
        if (create_inhibit_base_directory () < 0) {
                return CK_INHIBIT_ERROR_GENERAL;
        }

        /* create the named pipe and return it */
        return create_named_pipe (inhibit);
}
