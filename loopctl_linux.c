#include <dbus/dbus.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DBusConnection *connect_session_bus(void) {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "Connection error: %s\n", err.message);
    dbus_error_free(&err);
    return NULL;
  }
  return conn;
}

static char **list_mpris_names(DBusConnection *conn, int *out_count) {
  DBusError err;
  dbus_error_init(&err);

  DBusMessage *msg = dbus_message_new_method_call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "ListNames");
  if (!msg) {
    fprintf(stderr, "Failed to allocate message\n");
    return NULL;
  }

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "ListNames error: %s\n", err.message);
    dbus_error_free(&err);
    return NULL;
  }

  DBusMessageIter iter;
  dbus_message_iter_init(reply, &iter);

  if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "Unexpected reply type\n");
    dbus_message_unref(reply);
    return NULL;
  }

  DBusMessageIter array_iter;
  dbus_message_iter_recurse(&iter, &array_iter);

  char **names = NULL;
  const char *prefix = "org.mpris.MediaPlayer2.";
  size_t prefix_len = strlen(prefix);
  int count = 0;

  while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRING) {
    const char *name;
    dbus_message_iter_get_basic(&array_iter, &name);
    if (strncmp(name, prefix, prefix_len) == 0) {
      names = realloc(names, (count + 1) * sizeof(char *));
      names[count++] = strdup(name);
    }
    dbus_message_iter_next(&array_iter);
  }

  dbus_message_unref(reply);
  *out_count = count;
  return names;
}

static dbus_int64_t get_position(DBusConnection *conn, const char *bus_name) {
  DBusError err;
  dbus_error_init(&err);

  DBusMessage *msg =
      dbus_message_new_method_call(bus_name, "/org/mpris/MediaPlayer2",
                                   "org.freedesktop.DBus.Properties", "Get");
  if (!msg) {
    fprintf(stderr, "Failed to allocate Get message\n");
    return -1;
  }

  const char *iface = "org.mpris.MediaPlayer2.Player";
  const char *prop = "Position";
  dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                           &prop, DBUS_TYPE_INVALID);

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "[%s] Get(Position) error: %s\n", bus_name, err.message);
    dbus_error_free(&err);
    return -1;
  }

  DBusMessageIter iter;
  dbus_int64_t pos = -1;
  if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64) {
    dbus_message_iter_get_basic(&iter, &pos);
  } else {
    fprintf(stderr, "[%s] Unexpected reply type: %d\n", bus_name,
            dbus_message_iter_get_arg_type(&iter));
  }

  dbus_message_unref(reply);
  return pos;
}

static char *get_playback_status(DBusConnection *conn, const char *bus_name) {
  DBusError err;
  dbus_error_init(&err);

  DBusMessage *msg =
      dbus_message_new_method_call(bus_name, "/org/mpris/MediaPlayer2",
                                   "org.freedesktop.DBus.Properties", "Get");
  if (!msg) {
    fprintf(stderr, "Failed to allocate Get message\n");
    return NULL;
  }

  const char *iface = "org.mpris.MediaPlayer2.Player";
  const char *prop = "PlaybackStatus";
  dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                           &prop, DBUS_TYPE_INVALID);

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "[%s] Get(PlaybackStatus) error: %s\n", bus_name,
            err.message);
    dbus_error_free(&err);
    return NULL;
  }

  DBusMessageIter iter;
  dbus_message_iter_init(reply, &iter);
  if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
    fprintf(stderr, "[%s] Unexpected PlaybackStatus reply type\n", bus_name);
    dbus_message_unref(reply);
    return NULL;
  }

  DBusMessageIter var_iter;
  dbus_message_iter_recurse(&iter, &var_iter);
  char *status = NULL;
  if (dbus_message_iter_get_arg_type(&var_iter) == DBUS_TYPE_STRING) {
    const char *tmp;
    dbus_message_iter_get_basic(&var_iter, &tmp);
    status = strdup(tmp);
  } else {
    fprintf(stderr, "[%s] PlaybackStatus Variant is not STRING\n", bus_name);
  }

  dbus_message_unref(reply);
  return status;
}

int main(void) {
  printf("executing");
  DBusConnection *conn = connect_session_bus();
  if (!conn)
    return 1;

  int count = 0;
  char **players = list_mpris_names(conn, &count);
  if (!players)
    return 1;

  if (count == 0) {
    printf("No active MPRIS media players found.\n");
  } else {
    for (int i = 0; i < count; i++) {
      char *status = get_playback_status(conn, players[i]);
      dbus_int64_t pos = get_position(conn, players[i]);
      printf("%s: PlaybackStatus=%s, Position=%" PRId64 " µs (%.2f s)\n",
             players[i], status ? status : "unknown", pos,
             pos >= 0 ? pos / 1e6 : -1.0);
      free(players[i]);
      if (status)
        free(status);
    }
  }
  free(players);
  return 0;
}
