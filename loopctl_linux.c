#include <dbus/dbus.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// Fetch the current track’s object‑path (mpris:trackid) from Metadata
static char *get_track_id(DBusConnection *conn, const char *bus_name) {
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

static void set_position(DBusConnection *conn, const char *bus_name,
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

static volatile sig_atomic_t keep_running = 1;
void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

int main(void) {

  struct sigaction sa = {.sa_handler = handle_sigint, .sa_flags = 0};
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("sigaction");
    return EXIT_FAILURE;
  }

  DBusConnection *conn = connect_session_bus();
  if (!conn) {
    fprintf(stderr, "Failed to connect to session bus\n");
    return EXIT_FAILURE;
  }

  int count = 0;
  char **players = list_mpris_names(conn, &count);
  if (!players || count == 0) {
    printf("No active MPRIS media players found.\n");
    return EXIT_FAILURE;
  }

  int start_s, end_s;
  printf("Enter Start position (seconds): ");
  if (scanf("%d", &start_s) != 1) {
    fprintf(stderr, "Invalid input\n");
    return EXIT_FAILURE;
  }
  printf("Enter End position (seconds):   ");
  if (scanf("%d", &end_s) != 1) {
    fprintf(stderr, "Invalid input\n");
    return EXIT_FAILURE;
  }

  dbus_int64_t start_us = (dbus_int64_t)start_s * 1000000LL;
  dbus_int64_t end_us = (dbus_int64_t)end_s * 1000000LL;

  printf("Looping from %d to %d seconds indefinitely...\n", start_s, end_s);

  while (1) {
    for (int i = 0; i < count; i++) {
      char *status = get_playback_status(conn, players[i]);
      if (status) {
        if (strcmp(status, "Playing") == 0) {
          dbus_int64_t pos = get_position(conn, players[i]);
          if (pos >= end_us) {
            set_position(conn, players[i], start_us);
          }
        }
        free(status);
      }
    }
    usleep(100000); // 100 ms between checks
  }

  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);

  return EXIT_SUCCESS;
}
