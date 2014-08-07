/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager-openswan -- Network Manager Openswan plugin
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2010 - 2011 Red Hat, Inc.
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>

#include <glib/gi18n.h>

#include <nm-setting-vpn.h>
#include "nm-openswan-service.h"
#include "nm-utils.h"

#include <sys/types.h>

#if !defined(DIST_VERSION)
# define DIST_VERSION VERSION
#endif

static gboolean debug = FALSE;
GMainLoop *loop = NULL;

G_DEFINE_TYPE (NMOPENSWANPlugin, nm_openswan_plugin, NM_TYPE_VPN_PLUGIN)

typedef struct {
	GPid pid;
	char *secrets_path;
} NMOPENSWANPluginPrivate;

#define NM_OPENSWAN_PLUGIN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_OPENSWAN_PLUGIN, NMOPENSWANPluginPrivate))

#define NM_OPENSWAN_HELPER_PATH		LIBEXECDIR"/nm-openswan-service-helper"

typedef struct {
	const char *name;
	GType type;
	gint int_min;
	gint int_max;
} ValidProperty;

static ValidProperty valid_properties[] = {
	{ NM_OPENSWAN_RIGHT,                      G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_LEFTID,                     G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_LEFTXAUTHUSER,              G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_DOMAIN,                     G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_DHGROUP,                    G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_PFSGROUP,                   G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_DPDTIMEOUT,                 G_TYPE_INT, 0, 86400 },
	{ NM_OPENSWAN_IKE,                        G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_ESP,                        G_TYPE_STRING, 0, 0 },
	/* Ignored option for internal use */
	{ NM_OPENSWAN_PSK_INPUT_MODES,            G_TYPE_NONE, 0, 0 },
	{ NM_OPENSWAN_XAUTH_PASSWORD_INPUT_MODES, G_TYPE_NONE, 0, 0 },
	{ NM_OPENSWAN_PSK_VALUE "-flags",         G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_XAUTH_PASSWORD "-flags",    G_TYPE_STRING, 0, 0 },
	{ NULL,                                   G_TYPE_NONE, 0, 0 }
};

static ValidProperty valid_secrets[] = {
	{ NM_OPENSWAN_PSK_VALUE,                  G_TYPE_STRING, 0, 0 },
	{ NM_OPENSWAN_XAUTH_PASSWORD,             G_TYPE_STRING, 0, 0 },
	{ NULL,                                   G_TYPE_NONE, 0, 0 }
};

typedef struct ValidateInfo {
	ValidProperty *table;
	GError **error;
	gboolean have_items;
} ValidateInfo;

static void
validate_one_property (const char *key, const char *value, gpointer user_data)
{
	ValidateInfo *info = (ValidateInfo *) user_data;
	int i;

	if (*(info->error))
		return;

	info->have_items = TRUE;

	/* 'name' is the setting name; always allowed but unused */
	if (!strcmp (key, NM_SETTING_NAME))
		return;

	for (i = 0; info->table[i].name; i++) {
		ValidProperty prop = info->table[i];
		long int tmp;

		if (strcmp (prop.name, key))
			continue;

		switch (prop.type) {
		case G_TYPE_NONE:
			return; /* technically valid, but unused */
		case G_TYPE_STRING:
			return; /* valid */
		case G_TYPE_INT:
			errno = 0;
			tmp = strtol (value, NULL, 10);
			if (errno == 0 && tmp >= prop.int_min && tmp <= prop.int_max)
				return; /* valid */

			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             "invalid integer property '%s' or out of range [%d -> %d]",
			             key, prop.int_min, prop.int_max);
			break;
		case G_TYPE_BOOLEAN:
			if (!strcmp (value, "yes") || !strcmp (value, "no"))
				return; /* valid */

			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             "invalid boolean property '%s' (not yes or no)",
			             key);
			break;
		default:
			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             "unhandled property '%s' type %s",
			             key, g_type_name (prop.type));
			break;
		}
	}

	/* Did not find the property from valid_properties or the type did not match */
	if (!info->table[i].name) {
		g_set_error (info->error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "property '%s' invalid or not supported",
		             key);
	}
}

static gboolean
nm_openswan_properties_validate (NMSettingVPN *s_vpn, GError **error)
{
	ValidateInfo info = { &valid_properties[0], error, FALSE };

	nm_setting_vpn_foreach_data_item (s_vpn, validate_one_property, &info);
	if (!info.have_items) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "No VPN configuration options.");
		return FALSE;
	}

	return *error ? FALSE : TRUE;
}

static gboolean
nm_openswan_secrets_validate (NMSettingVPN *s_vpn, GError **error)
{
	ValidateInfo info = { &valid_secrets[0], error, FALSE };

	nm_setting_vpn_foreach_secret (s_vpn, validate_one_property, &info);
	if (!info.have_items) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "No VPN secrets!");
		return FALSE;
	}

	return *error ? FALSE : TRUE;
}

/****************************************************************/

static const char *ipsec_paths[] =
{
	"/usr/sbin/ipsec",
	"/sbin/ipsec",
	"/usr/local/sbin/ipsec",
	NULL
};

static const char *
find_ipsec (GError **error)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (ipsec_paths); i++) {
		if (g_file_test (ipsec_paths[i], G_FILE_TEST_EXISTS))
			return ipsec_paths[i];
	}

	g_set_error_literal (error,
	                     NM_VPN_PLUGIN_ERROR,
	                     NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
	                     "Could not find ipsec binary.");
	return NULL;
}

static void
pluto_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMOPENSWANPlugin *plugin = NM_OPENSWAN_PLUGIN (user_data);
	NMOPENSWANPluginPrivate *priv = NM_OPENSWAN_PLUGIN_GET_PRIVATE (plugin);
	guint error = 0;

	if (debug)
		g_message ("pluto_watch: current child pid = %d, pluto pid=%d", pid, priv->pid);

	if (WIFEXITED (status)) {
		error = WEXITSTATUS (status);
		if (error != 0) {
			g_warning ("pluto_watch: pluto exited with error code %d", error);
			nm_vpn_plugin_failure (NM_VPN_PLUGIN (plugin), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		}
	} else if (WIFSTOPPED (status))
		g_warning ("pluto_watch: pluto stopped unexpectedly with signal %d", WSTOPSIG (status));
	else if (WIFSIGNALED (status))
		g_warning ("pluto_watch: pluto died with signal %d", WTERMSIG (status));
	else
		g_warning ("pluto_watch: pluto died from an unknown cause");

	/* Reap child if needed. */
	waitpid (pid, NULL, WNOHANG);

	if (pid == priv->pid) {
		priv->pid = 0;
		if (debug)
			g_message ("pluto_watch: nm pluto service is stopping");
	} else {
		if (debug)
			g_message ("pluto_watch: nm pluto service will continue after reaping a child");
	}

	g_spawn_close_pid (pid);
}

static gboolean do_spawn (gboolean dont_reap_child,
                          GPid *out_pid,
                          int *out_stdin,
                          GError **error,
                          const char *progname,
                          ...) G_GNUC_NULL_TERMINATED;

static gboolean
do_spawn (gboolean dont_reap_child,
          GPid *out_pid,
          int *out_stdin,
          GError **error,
          const char *progname,
          ...)
{
	GError *local = NULL;
	va_list ap;
	GPtrArray *argv;
	char *cmdline, *arg;
	gboolean success;

	argv = g_ptr_array_sized_new (10);
	g_ptr_array_add (argv, (char *) progname);

	va_start (ap, progname);
	while ((arg = va_arg (ap, char *)))
		g_ptr_array_add (argv, arg);
	va_end (ap);
	g_ptr_array_add (argv, NULL);

	if (debug) {
		cmdline = g_strjoinv (" ", (char **) argv->pdata);
		g_message ("Spawning: %s", cmdline);
		g_free (cmdline);
	}

	if (out_stdin) {
		success = g_spawn_async_with_pipes (NULL, (char **) argv->pdata, NULL,
		                                    dont_reap_child ? G_SPAWN_DO_NOT_REAP_CHILD : 0,
		                                    NULL, NULL, out_pid, out_stdin,
		                                    NULL, NULL, &local);
	} else {
		success = g_spawn_async (NULL, (char **) argv->pdata, NULL,
		                         dont_reap_child ? G_SPAWN_DO_NOT_REAP_CHILD : 0,
		                         NULL, NULL, out_pid, &local);
	}
	if (!success) {
		g_warning ("Spawn failed: (%s/%d) %s",
		           g_quark_to_string (local->domain),
		           local->code, local->message);
		g_propagate_error (error, local);
	}

	g_ptr_array_free (argv, TRUE);
	return success;
}

static gint
nm_openswan_start_openswan_binary (NMOPENSWANPlugin *plugin,
                                   const char *con_name,
                                   GError **error)
{
	NMOPENSWANPluginPrivate *priv = NM_OPENSWAN_PLUGIN_GET_PRIVATE (plugin);
	const char *ipsec_binary;
	gint stdin_fd = -1;
	GPid pid_auto;

	ipsec_binary = find_ipsec (error);
	if (!ipsec_binary)
		return -1;

	/* Start the IPSec service */
	if (!do_spawn (TRUE, &priv->pid, NULL, error, ipsec_binary, "setup", "start", NULL))
		return -1;

	g_message ("ipsec/pluto started with pid %d", priv->pid);
	g_child_watch_add (priv->pid, (GChildWatchFunc) pluto_watch_cb, plugin);
	sleep (2);

	/* Start the helper we write the connection configuration to */
	if (!do_spawn (TRUE, &pid_auto, &stdin_fd, error,
	               ipsec_binary, "auto", "--add", "--config", "-", con_name, NULL)) {
		return -1;
	}

	if (debug)
		g_message ("pluto auto started with pid %d", pid_auto);

	g_child_watch_add (pid_auto, (GChildWatchFunc) pluto_watch_cb, plugin);

	return stdin_fd;
}

static gint
nm_openswan_start_openswan_connection (NMOPENSWANPlugin *plugin,
                                       const char *con_name,
                                       GError **error)
{
	GPid pid;
	const char *ipsec_binary;
	gint stdin_fd = -1;

	ipsec_binary = find_ipsec (error);
	if (!ipsec_binary)
		return -1;

	if (!do_spawn (TRUE, &pid, &stdin_fd, error, ipsec_binary, "auto", "--up", con_name, NULL))
		return -1;

	if (debug)
		g_message ("pluto up started with pid %d", pid);

	g_child_watch_add (pid, (GChildWatchFunc) pluto_watch_cb, plugin);

	return stdin_fd;
}

static inline void
write_config_option (int fd, const char *format, ...)
{
	char *string;
	va_list args;

	va_start (args, format);
	string = g_strdup_vprintf (format, args);

	if (debug)
		g_print ("Config: %s", string);

	if ( write (fd, string, strlen (string)) == -1)
		g_warning ("nm-openswan: error in write_config_option");

	g_free (string);
	va_end (args);
}

static gboolean
nm_openswan_config_write (gint fd,
                          const char *con_name,
                          NMSettingVPN *s_vpn,
                          GError **error)
{
	const char *props_username;
	const char *default_username;
	const char *phase1_alg_str;
	const char *phase2_alg_str;

	g_assert (fd >= 0);

	write_config_option (fd, "conn %s\n", con_name);
	write_config_option (fd, " aggrmode=yes\n");
	write_config_option (fd, " authby=secret\n");
	write_config_option (fd, " left=%%defaultroute\n");
	write_config_option (fd, " leftid=@%s\n", nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_LEFTID));
	write_config_option (fd, " leftxauthclient=yes\n");
	write_config_option (fd, " leftmodecfgclient=yes\n");

	default_username = nm_setting_vpn_get_user_name (s_vpn);
	props_username = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_LEFTXAUTHUSER);
	if (   default_username && strlen (default_username)
		&& (!props_username || !strlen (props_username)))
		write_config_option (fd, " leftxauthusername=%s\n", default_username);
	else
		write_config_option (fd, " leftxauthusername=%s\n", props_username);

	write_config_option (fd, " right=%s\n", nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_RIGHT));
	write_config_option (fd, " remote_peer_type=cisco\n");
	write_config_option (fd, " rightxauthserver=yes\n");
	write_config_option (fd, " rightmodecfgserver=yes\n");

	phase1_alg_str = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_IKE);
	if (!phase1_alg_str || !strlen (phase1_alg_str))
		write_config_option (fd, " ike=aes-sha1\n");
	else
		write_config_option (fd, " ike=%s\n", phase1_alg_str);

	phase2_alg_str = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_ESP);
	if (!phase2_alg_str || !strlen (phase2_alg_str))
		write_config_option (fd, " esp=aes-sha1;modp1024\n");
	else
		write_config_option (fd, " esp=%s\n", phase2_alg_str);

	write_config_option (fd, " nm_configured=yes\n");
	write_config_option (fd, " rekey=yes\n");
	write_config_option (fd, " salifetime=24h\n");
	write_config_option (fd, " ikelifetime=24h\n");
	write_config_option (fd, " keyingtries=1\n");
	write_config_option (fd, " auto=add\n");

	close (fd);
	sleep (3);
	return TRUE;
}

static gboolean
nm_openswan_config_psk_write (NMSettingVPN *s_vpn,
                              const char *secrets_path,
                              GError **error)
{
	const char *pw_type, *psk, *leftid;
	int fd;

	/* Check for ignored group password */
	pw_type = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_PSK_INPUT_MODES);
	if (pw_type && !strcmp (pw_type, NM_OPENSWAN_PW_TYPE_UNUSED))
		return TRUE;

	psk = nm_setting_vpn_get_secret (s_vpn, NM_OPENSWAN_PSK_VALUE);
	if (!psk)
		return TRUE;

	/* Write the PSK */
	errno = 0;
	fd = open (secrets_path, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             "Failed to open secrets file: (%d) %s.",
		             errno, g_strerror (errno));
		return FALSE;
	}

	leftid = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_LEFTID);
	g_assert (leftid);
	write_config_option (fd, "@%s: PSK \"%s\"\n", leftid, psk);

	close (fd);
	return TRUE;
}

static void
delete_secrets_file (NMOPENSWANPlugin *self)
{
	NMOPENSWANPluginPrivate *priv = NM_OPENSWAN_PLUGIN_GET_PRIVATE (self);

	if (priv->secrets_path) {
		unlink (priv->secrets_path);
		g_clear_pointer (&priv->secrets_path, g_free);
	}
}


static gboolean
real_connect (NMVPNPlugin   *plugin,
              NMConnection  *connection,
              GError       **error)
{
	NMOPENSWANPlugin *self = NM_OPENSWAN_PLUGIN (plugin);
	NMOPENSWANPluginPrivate *priv = NM_OPENSWAN_PLUGIN_GET_PRIVATE (self);
	NMSettingVPN *s_vpn;
	const char *con_name = nm_connection_get_uuid (connection);
	gint fd = -1;

	s_vpn = nm_connection_get_setting_vpn (connection);
	g_assert (s_vpn);

	if (!nm_openswan_properties_validate (s_vpn, error))
		return FALSE;

	if (!nm_openswan_secrets_validate (s_vpn, error))
		return FALSE;

	/* Write the IPSec secret (group password) */
	priv->secrets_path = g_strdup_printf (SYSCONFDIR "/ipsec.d/ipsec-%s.secrets", con_name);
	if (!nm_openswan_config_psk_write (s_vpn, priv->secrets_path, error))
		return FALSE;

	fd = nm_openswan_start_openswan_binary (self, con_name, error);
	if (fd < 0)
		goto error;

	if (debug)
		nm_connection_dump (connection);

	/* Start the IPSec service */
	if (!nm_openswan_config_write (fd, con_name, s_vpn, error))
		goto error;
	close (fd);

	/* Start the actual IPSec connection */
	fd = nm_openswan_start_openswan_connection (self, con_name, error);
	if (fd < 0)
		goto error;

	/* Write the user password */
	write_config_option (fd, "%s", nm_setting_vpn_get_secret (s_vpn, NM_OPENSWAN_XAUTH_PASSWORD));
	close (fd);
	return TRUE;

error:
	if (fd >= 0)
		close (fd);
	delete_secrets_file (self);
	return FALSE;
}

static gboolean
real_need_secrets (NMVPNPlugin *plugin,
                   NMConnection *connection,
                   char **setting_name,
                   GError **error)
{
	NMSettingVPN *s_vpn;
	const char *pw_type;

	g_return_val_if_fail (NM_IS_VPN_PLUGIN (plugin), FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

	s_vpn = NM_SETTING_VPN (nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN));
	if (!s_vpn) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_CONNECTION_INVALID,
		                     "Could not process the request because the VPN connection settings were invalid.");
		return FALSE;
	}

	pw_type = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_PSK_INPUT_MODES);
	if (!pw_type || strcmp (pw_type, NM_OPENSWAN_PW_TYPE_UNUSED)) {
		if (!nm_setting_vpn_get_secret (s_vpn, NM_OPENSWAN_PSK_VALUE)) {
			*setting_name = NM_SETTING_VPN_SETTING_NAME;
			return TRUE;
		}
	}

	pw_type = nm_setting_vpn_get_data_item (s_vpn, NM_OPENSWAN_XAUTH_PASSWORD_INPUT_MODES);
	if (!pw_type || strcmp (pw_type, NM_OPENSWAN_PW_TYPE_UNUSED)) {
		if (!nm_setting_vpn_get_secret (s_vpn, NM_OPENSWAN_XAUTH_PASSWORD)) {
			*setting_name = NM_SETTING_VPN_SETTING_NAME;
			return TRUE;
		}
	}

	return FALSE;
}

static gboolean
real_disconnect (NMVPNPlugin *plugin, GError **error)
{
	const char *ipsec_binary;

	delete_secrets_file (NM_OPENSWAN_PLUGIN (plugin));

	ipsec_binary = find_ipsec (error);
	if (!ipsec_binary)
		return FALSE;

	return do_spawn (FALSE, NULL, NULL, error, ipsec_binary, "setup", "stop", NULL);
}

static void
nm_openswan_plugin_init (NMOPENSWANPlugin *plugin)
{
}

static void
finalize (GObject *object)
{
	delete_secrets_file (NM_OPENSWAN_PLUGIN (object));

	G_OBJECT_CLASS (nm_openswan_plugin_parent_class)->finalize (object);
}

static void
nm_openswan_plugin_class_init (NMOPENSWANPluginClass *openswan_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (openswan_class);
	NMVPNPluginClass *parent_class = NM_VPN_PLUGIN_CLASS (openswan_class);

	g_type_class_add_private (object_class, sizeof (NMOPENSWANPluginPrivate));

	/* virtual methods */
	object_class->finalize = finalize;
	parent_class->connect = real_connect;
	parent_class->need_secrets = real_need_secrets;
	parent_class->disconnect = real_disconnect;
}

NMOPENSWANPlugin *
nm_openswan_plugin_new (void)
{
	return (NMOPENSWANPlugin *) g_object_new (NM_TYPE_OPENSWAN_PLUGIN,
	                                          NM_VPN_PLUGIN_DBUS_SERVICE_NAME, NM_DBUS_SERVICE_OPENSWAN,
	                                          NULL);
}

static void
signal_handler (int signo)
{
	if (signo == SIGINT || signo == SIGTERM)
		g_main_loop_quit (loop);
}

static void
setup_signals (void)
{
	struct sigaction action;
	sigset_t mask;

	sigemptyset (&mask);
	action.sa_handler = signal_handler;
	action.sa_mask = mask;
	action.sa_flags = 0;
	sigaction (SIGTERM,  &action, NULL);
	sigaction (SIGINT,  &action, NULL);
}

static void
quit_mainloop (NMOPENSWANPlugin *plugin, gpointer user_data)
{
	g_main_loop_quit ((GMainLoop *) user_data);
}

int
main (int argc, char *argv[])
{
	NMOPENSWANPlugin *plugin;
	gboolean persist = FALSE;
	GOptionContext *opt_ctx = NULL;

	GOptionEntry options[] = {
		{ "persist", 0, 0, G_OPTION_ARG_NONE, &persist, N_("Don't quit when VPN connection terminates"), NULL },
		{ "debug", 0, 0, G_OPTION_ARG_NONE, &debug, N_("Enable verbose debug logging (may expose passwords)"), NULL },
		{NULL}
	};

#if !GLIB_CHECK_VERSION (2, 35, 0)
	g_type_init ();
#endif

	/* locale will be set according to environment LC_* variables */
	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, NM_OPENSWAN_LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Parse options */
	opt_ctx = g_option_context_new (NULL);
	g_option_context_set_translation_domain (opt_ctx, GETTEXT_PACKAGE);
	g_option_context_set_ignore_unknown_options (opt_ctx, FALSE);
	g_option_context_set_help_enabled (opt_ctx, TRUE);
	g_option_context_add_main_entries (opt_ctx, options, NULL);

	g_option_context_set_summary (opt_ctx,
		_("nm-openswan-service provides integrated IPsec VPN capability to NetworkManager."));

	g_option_context_parse (opt_ctx, &argc, &argv, NULL);
	g_option_context_free (opt_ctx);

	if (getenv ("OPENSWAN_DEBUG") || getenv ("IPSEC_DEBUG"))
		debug = TRUE;

	if (debug)
		g_message ("%s (version " DIST_VERSION ") starting...", argv[0]);

	plugin = nm_openswan_plugin_new ();
	if (!plugin)
		exit (1);

	loop = g_main_loop_new (NULL, FALSE);

	if (!persist)
		g_signal_connect (plugin, "quit", G_CALLBACK (quit_mainloop), loop);

	setup_signals ();
	g_main_loop_run (loop);

	g_main_loop_unref (loop);
	g_object_unref (plugin);

	exit (0);
}
