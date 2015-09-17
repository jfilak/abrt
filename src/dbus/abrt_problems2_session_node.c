/*
  Copyright (C) 2015  ABRT team

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "abrt_problems2_session_node.h"
#include "abrt_problems2_service.h"
#include "abrt-polkit.h"

#include "libabrt.h"

#include <assert.h>

struct p2s_node
{
    char   *p2s_path;
    char   *p2s_caller;
    uid_t   p2s_uid;
    int     p2s_state;
    time_t  p2s_stamp;
    guint   p2s_regid;
};

enum
{
    P2S_STATE_INIT,
    P2S_STATE_AUTH,
};

static void p2s_node_free(struct p2s_node *node)
{
    if (NULL == node)
        return;

    free(node->p2s_path);
    node->p2s_path = (void *)0xDEADBEEF;

    free(node->p2s_caller);
    node->p2s_caller = (void *)0xDEADBEEF;
}

static GHashTable *s_p2s_nodes;

static GHashTable *nodes_table(void)
{
    if (s_p2s_nodes == NULL)
        s_p2s_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)p2s_node_free);

    return s_p2s_nodes;
}

static struct p2s_node *get_session(const gchar *caller,
                             uid_t caller_uid,
                             const gchar *path,
                             GError      **error)
{
    struct p2s_node *node = abrt_problems2_session_find_node(caller);
    if (node != NULL)
    {
        if (   strcmp(node->p2s_caller, caller) != 0
            || strcmp(node->p2s_path, path) != 0
            || node->p2s_uid != caller_uid)
        {
            log_warning("Problems2 Session object does not belong to UID %d", caller_uid);

            g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Your Problems2 Session is broken. Check system logs for more details.");

            return NULL;
        }
    }

    return node;
}

static void change_state(struct p2s_node* node, int new_state, GDBusConnection *connection)
{
    if (node->p2s_state == new_state)
        return;

    int value = -1;
    int old_state = node->p2s_state;
    node->p2s_state = new_state;

    if      (old_state == P2S_STATE_INIT && new_state == P2S_STATE_AUTH)
        value = 0;
    else if (old_state == P2S_STATE_AUTH && new_state == P2S_STATE_INIT)
        value = 1;
    else
        goto forgotten_state;

    GError *error = NULL;
    GVariant *parameters = g_variant_new("(i)", value);

    log("Emitting signal: %s, %s, org.freedesktop.Problems2.Session, AuthorizationChanged", ABRT_P2_BUS, node->p2s_path);

    GDBusMessage *message = g_dbus_message_new_signal (node->p2s_path,
                                         ABRT_P2_NS_MEMBER("Session"),
                                         "AuthorizationChanged");

    g_dbus_message_set_sender(message, ABRT_P2_BUS);
    g_dbus_message_set_body(message, parameters);
    g_dbus_connection_send_message(connection, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error);
    if (error != NULL)
    {
        error_msg("Failed to emit 'AuthorizationChanged': %s", error->message);
        g_free(error);
    }

    return;

forgotten_state:
    error_msg("BUG: unsupported state, current : %d, new : %d", node->p2s_state, new_state);
}

/* D-Bus method handler
 */
static void dbus_method_call(GDBusConnection *connection,
                        const gchar *caller,
                        const gchar *object_path,
                        const gchar *interface_name,
                        const gchar *method_name,
                        GVariant    *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer    user_data)
{
    /* Check sanity */
    if (strcmp(interface_name, "org.freedesktop.Problems2.Session") != 0)
    {
        error_msg("Unsupported interface %s", interface_name);
        return;
    }

    //GVariant *response;
    GError *error = NULL;

    uid_t caller_uid = abrt_problems2_service_caller_real_uid(connection, caller, &error);
    if (caller_uid == (uid_t)-1)
    {
        g_dbus_method_invocation_return_gerror(invocation, error);
        return;
    }

    struct p2s_node *node = get_session(caller, caller_uid, object_path, &error);
    if (node == NULL)
    {
        g_dbus_method_invocation_return_gerror(invocation, error);
        return;
    }

    if (strcmp("Authorize", method_name) == 0)
    {
        int retval = -1;

        const gchar *window_id = NULL;
        gint32 flags;

        g_variant_get(parameters, "(&si)", &window_id, &flags);

        if ((flags & 1) && (flags & 2))
        {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                      "org.freedesktop.problems.InvalidArguments",
                                      "You must use either 0x1 or 0x2.");
            return;
        }

        if ((flags == 0) || (flags & (~3)))
        {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                      "org.freedesktop.problems.InvalidArguments",
                                      "Unsupported flags. You must use either 0x1 or 0x2.");
            return;
        }

        if (flags & 1)
        {
            switch(node->p2s_state)
            {
                case P2S_STATE_INIT:
                    if (polkit_check_authorization_dname(caller, "org.freedesktop.problems.getall") == PolkitYes)
                    {
                        change_state(node, P2S_STATE_AUTH, connection);
                        retval = 0;
                    }
                    break;

                case P2S_STATE_AUTH:
                    retval = 0;
                    break;
            }
        }

        if (flags & 2)
        {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                      "org.freedesktop.problems.NotYetImplemented",
                                      "0x2 is not yet implemented.");
            return;
        }

        GVariant *response = g_variant_new("(i)", retval);
        g_dbus_method_invocation_return_value(invocation, response);
        return;
    }

    if (strcmp("Close", method_name) == 0)
    {
        switch(node->p2s_state)
        {
            case P2S_STATE_AUTH:
                change_state(node, P2S_STATE_INIT, connection);
                break;

            case P2S_STATE_INIT:
                /* pass */
                break;
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

        /* TODO: This should be handled by the service module */
        g_dbus_connection_unregister_object(connection, node->p2s_regid);

        g_hash_table_remove(nodes_table(), node->p2s_caller);

        return;
    }

    error_msg("BUG: org.freedesktop.Problems2.Session does not have method: %s", method_name);
}

static GVariant *dbus_get_property(GDBusConnection *connection,
                        const gchar *caller,
                        const gchar *object_path,
                        const gchar *interface_name,
                        const gchar *property_name,
                        GError      **error,
                        gpointer    user_data)
{
    if (strcmp(interface_name, "org.freedesktop.Problems2.Session") != 0)
    {
        error_msg("Unsupported interface %s", interface_name);
        return NULL;
    }

    if (strcmp("is_authorized", property_name))
    {
        error_msg("Unsupported property %s", property_name);
        return NULL;
    }

    uid_t caller_uid = abrt_problems2_service_caller_real_uid(connection, caller, error);
    if (caller_uid == (uid_t)-1)
        return NULL;

    struct p2s_node *node = get_session(caller, caller_uid, object_path, error);
    if (node == NULL)
        return NULL;

    return g_variant_new_boolean(abrt_problems2_session_is_authorized(node));
}

GDBusInterfaceVTable *abrt_problems2_session_node_vtable(void)
{
    static GDBusInterfaceVTable default_vtable =
    {
        .method_call = dbus_method_call,
        .get_property = dbus_get_property,
        .set_property = NULL,
    };

    return &default_vtable;
}

/* Public interface */

struct p2s_node *abrt_problems2_session_new_node(char *path, char *caller, uid_t uid, guint regid)
{
    {
        struct p2s_node *new_node_dup = abrt_problems2_session_find_node(caller);
        assert(new_node_dup == NULL);
    }

    struct p2s_node *node = xmalloc(sizeof(*node));
    node->p2s_path = path;
    node->p2s_caller = caller;
    node->p2s_uid = uid;
    node->p2s_regid = regid;

    if (node->p2s_uid == 0)
        node->p2s_state = P2S_STATE_AUTH;
    else
        node->p2s_state = P2S_STATE_INIT;

    node->p2s_stamp = time(NULL);

    g_hash_table_insert(nodes_table(), caller, node);

    return node;
}

const char *abrt_problems2_session_node_path(struct p2s_node *session)
{
    return session->p2s_path;
}

guint abrt_problems2_session_node_registration_id(struct p2s_node *session)
{
    return session->p2s_regid;
}

struct p2s_node *abrt_problems2_session_find_node(const char *caller)
{
    return (struct p2s_node *)g_hash_table_lookup(nodes_table(), caller);
}

int abrt_problems2_session_is_authorized(struct p2s_node *session)
{
    return session->p2s_state == P2S_STATE_AUTH;
}
