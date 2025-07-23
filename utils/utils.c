#include "utils.h"
#include <dbus/dbus.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DBusConnection *connect_session_bus(void) {
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

char **list_mpris_names(DBusConnection *conn, int *out_count) {
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

dbus_int64_t get_position(DBusConnection *conn, const char *bus_name) {
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

char *get_playback_status(DBusConnection *conn, const char *bus_name) {
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

// Fetch the current track’s object‑path (mpris:trackid) from Metadata
char *get_track_id(DBusConnection *conn, const char *bus_name) {
  DBusError err;
  dbus_error_init(&err);

  DBusMessage *msg =
      dbus_message_new_method_call(bus_name, "/org/mpris/MediaPlayer2",
                                   "org.freedesktop.DBus.Properties", "Get");
  if (!msg) {
    fprintf(stderr, "[%s] Failed to allocate Get(Metadata) message\n",
            bus_name);
    return NULL;
  }

  const char *iface = "org.mpris.MediaPlayer2.Player";
  const char *prop = "Metadata";
  dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                           &prop, DBUS_TYPE_INVALID);

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "[%s] Get(Metadata) error: %s\n", bus_name, err.message);
    dbus_error_free(&err);
    return NULL;
  }

  DBusMessageIter iter;
  dbus_message_iter_init(reply, &iter);
  if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
    fprintf(stderr, "[%s] Unexpected Metadata reply type %d\n", bus_name,
            dbus_message_iter_get_arg_type(&iter));
    dbus_message_unref(reply);
    return NULL;
  }

  DBusMessageIter var_iter;
  dbus_message_iter_recurse(&iter, &var_iter);
  if (dbus_message_iter_get_arg_type(&var_iter) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "[%s] Metadata variant not an array\n", bus_name);
    dbus_message_unref(reply);
    return NULL;
  }

  DBusMessageIter array_iter;
  dbus_message_iter_recurse(&var_iter, &array_iter);
  char *track_id = NULL;

  while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry_iter;
    dbus_message_iter_recurse(&array_iter, &entry_iter);

    const char *key;
    if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&entry_iter, &key);
      if (strcmp(key, "mpris:trackid") == 0) {
        dbus_message_iter_next(&entry_iter);
        if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
          DBusMessageIter val_iter;
          dbus_message_iter_recurse(&entry_iter, &val_iter);
          if (dbus_message_iter_get_arg_type(&val_iter) ==
              DBUS_TYPE_OBJECT_PATH) {
            const char *tmp;
            dbus_message_iter_get_basic(&val_iter, &tmp);
            track_id = strdup(tmp);
          }
        }
        break;
      }
    }
    dbus_message_iter_next(&array_iter);
  }

  dbus_message_unref(reply);
  return track_id;
}

void set_position(DBusConnection *conn, const char *bus_name,
                  dbus_int64_t position) {

  char *track_id = get_track_id(conn, bus_name);
  if (!track_id) {
    fprintf(stderr, "[%s] Cannot set position: no track ID\n", bus_name);
    return;
  }

  DBusError err;
  dbus_error_init(&err);

  DBusMessage *msg = dbus_message_new_method_call(
      bus_name, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
      "SetPosition");
  if (!msg) {
    fprintf(stderr, "[%s] Failed to allocate SetPosition message\n", bus_name);
    free(track_id);
    return;
  }

  dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &track_id,
                           DBUS_TYPE_INT64, &position, DBUS_TYPE_INVALID);

  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);

  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "[%s] SetPosition error: %s\n", bus_name, err.message);
    dbus_error_free(&err);
  }

  if (reply)
    dbus_message_unref(reply);
  free(track_id);
}

void print_dbus_error(const char *str, DBusError *err) {
  fprintf(stderr, "%s: %s\n", str, err->message);
}

dbus_int64_t get_track_length(DBusConnection *conn, const char *bus_name) {
  DBusMessage *msg;
  DBusMessage *reply;
  DBusError err;
  DBusMessageIter args, variant, dict;

  const char *track_id = get_track_id(conn, bus_name);
  if (!track_id) {
    fprintf(stderr, "Track ID is NULL\n");
    return -1;
  }

  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (dbus_error_is_set(&err)) {
    print_dbus_error("Connection Error", &err);
    dbus_error_free(&err);
    return -1;
  }

  msg = dbus_message_new_method_call(bus_name, "/org/mpris/MediaPlayer2",
                                     "org.freedesktop.DBus.Properties", "Get");
  if (!msg) {
    fprintf(stderr, "Message Null\n");
    return -1;
  }

  const char *iface = "org.mpris.MediaPlayer2.Player";
  const char *prop = "Metadata";

  dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                           &prop, DBUS_TYPE_INVALID);

  reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);
  if (dbus_error_is_set(&err)) {
    print_dbus_error("Reply Error", &err);
    dbus_error_free(&err);
    return -1;
  }

  if (!reply) {
    fprintf(stderr, "Reply was NULL\n");
    return -1;
  }

  if (!dbus_message_iter_init(reply, &args)) {
    fprintf(stderr, "Reply has no arguments\n");
    dbus_message_unref(reply);
    return -1;
  }

  dbus_message_iter_recurse(&args, &variant); // variant of a{sv}
  if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
    fprintf(stderr, "Metadata is not a dict\n");
    dbus_message_unref(reply);
    return -1;
  }

  dbus_message_iter_recurse(&variant, &dict);

  while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry, key_iter, value_iter;
    const char *key;

    dbus_message_iter_recurse(&dict, &entry);
    dbus_message_iter_recurse(&entry, &key_iter);

    if (dbus_message_iter_get_arg_type(&key_iter) != DBUS_TYPE_STRING) {
      dbus_message_iter_next(&dict);
      continue;
    }

    dbus_message_iter_get_basic(&key_iter, &key);
    dbus_message_iter_next(&entry);

    if (strcmp(key, "mpris:length") == 0) {
      dbus_message_iter_recurse(&entry, &value_iter);
      if (dbus_message_iter_get_arg_type(&value_iter) == DBUS_TYPE_INT64) {
        dbus_int64_t length;
        dbus_message_iter_get_basic(&value_iter, &length);
        dbus_message_unref(reply);
        return length;
      }
    }

    dbus_message_iter_next(&dict);
  }

  dbus_message_unref(reply);
  fprintf(stderr, "mpris:length not found in metadata\n");
  return -1;
}
