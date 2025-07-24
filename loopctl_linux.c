#include "utils/utils.h"
#include <dbus/dbus.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;
static DBusConnection *conn;
static const char *player; // the MPRIS player we’re looping
static int timer_fd;
static int64_t start_us, end_us;
static long loops_left = -1;

static void arm_timer(int64_t now_us) {
  if (now_us >= end_us)
    now_us = start_us;
  int64_t delta = end_us - now_us;
  struct itimerspec its = {.it_value = {
                               .tv_sec = delta / 1000000,
                               .tv_nsec = (delta % 1000000) * 1000,
                           }};
  if (timerfd_settime(timer_fd, 0, &its, NULL) < 0)
    perror("timerfd_settime");
}

static inline void on_segment_finished(void) {
  if (loops_left == 0) {
    // we're done – stop the timer and break the main loop
    struct itimerspec off = {{0}, {0}};
    timerfd_settime(timer_fd, 0, &off, NULL);
    keep_running = 0;
  } else {
    if (loops_left > 0)
      --loops_left;
    set_position(conn, player, start_us);
    int64_t pos = get_position(conn, player);
    arm_timer(pos);
  }
}

void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

static DBusHandlerResult filter_cb(DBusConnection *c, DBusMessage *m,
                                   void *userdata) {
  (void)c;
  (void)userdata;

  if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties",
                             "PropertiesChanged")) {
    const char *iface = NULL;
    DBusMessageIter iter;

    if (!dbus_message_iter_init(m, &iter))
      return DBUS_HANDLER_RESULT_HANDLED;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
      return DBUS_HANDLER_RESULT_HANDLED;

    dbus_message_iter_get_basic(&iter, &iface);
    if (strcmp(iface, "org.mpris.MediaPlayer2.Player") != 0)
      return DBUS_HANDLER_RESULT_HANDLED;

    // map is next argument
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
      return DBUS_HANDLER_RESULT_HANDLED;

    DBusMessageIter dict;
    dbus_message_iter_recurse(&iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
      DBusMessageIter entry, variant;
      const char *prop = NULL;

      dbus_message_iter_recurse(&dict, &entry); // dict entry (key, value)
      if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
        dbus_message_iter_next(&dict);
        continue;
      }

      dbus_message_iter_get_basic(&entry, &prop);
      if (strcmp(prop, "PlaybackStatus") == 0) {
        dbus_message_iter_next(&entry); // move to variant
        dbus_message_iter_recurse(&entry, &variant);
        const char *status = NULL;
        dbus_message_iter_get_basic(&variant, &status);

        if (status && strcmp(status, "Playing") == 0) {
          int64_t pos = get_position(conn, player);
          arm_timer(pos);
        } else {
          struct itimerspec off = {{0}, {0}};
          timerfd_settime(timer_fd, 0, &off, NULL);
        }
      }

      dbus_message_iter_next(&dict);
    }

  } else if (dbus_message_is_signal(m, "org.mpris.MediaPlayer2.Player",
                                    "Seeked")) {
    int64_t new_pos = 0;
    if (dbus_message_get_args(m, NULL, DBUS_TYPE_INT64, &new_pos,
                              DBUS_TYPE_INVALID)) {
      if (new_pos >= end_us) {
        // user dragged beyond our end – snap back but DO NOT change loops_left
        // here
        set_position(conn, player, start_us);
        int64_t pos = get_position(conn, player);
        arm_timer(pos);
      } else {
        arm_timer(new_pos);
      }
    }
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

void usage() {
  char *prog = "loopctl";
  fprintf(stderr,
          "Usage:\n"
          "  %s                 # loop full track infinitely\n"
          "  %s N               # loop full track N times\n"
          "  %s -p START END    # loop [START,END] infinitely\n"
          "  %s -p START END X  # loop [START,END] X times\n"
          "  (START, END in seconds)\n",
          prog, prog, prog, prog);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_sigint);

  conn = connect_session_bus();
  if (!conn) {
    fprintf(stderr, "Failed to connect to session bus\n");
    return EXIT_FAILURE;
  }

  int count = 0;
  char **players = list_mpris_names(conn, &count);
  if (!players || count == 0) {
    fprintf(stderr, "No active MPRIS media players found.\n");
    return EXIT_FAILURE;
  }

  player = NULL;
  for (int i = 0; i < count; i++) {
    char *status = get_playback_status(conn, players[i]);
    if (status) {
      if (strcmp(status, "Playing") == 0) {
        player = players[i];
        free(status);
        break;
      }
    }
    free(status);
  }

  if (!player) {
    fprintf(stderr, "No playing MPRIS media player found.\n");
    for (int i = 0; i < count; i++)
      free(players[i]);
    free(players);
    return EXIT_FAILURE;
  }

  // Parse command line arguments
  if (argc == 1) {
    // loopctl - full, infinite
    start_us = 0;
    end_us = get_track_length(conn, player);

    while (keep_running) {
      set_position(conn, player, start_us);
      usleep((end_us - start_us) / 1000000 *
             1000000); // Wait for track duration
    }
  } else if (argc == 2) {
    // loopctl 5 - full, 5 times
    int times = atoi(argv[1]);
    start_us = 0;
    end_us = get_track_length(conn, player);

    for (int loop = 0; loop < times && keep_running; loop++) {
      set_position(conn, player, start_us);
      usleep((end_us - start_us) / 1000000 *
             1000000); // Wait for track duration
    }
  } else if (argc == 4) {
    // loopctl part START END - partial, infinite
    int s = atoi(argv[2]);
    int e = atoi(argv[3]);
    start_us = (int64_t)s * 1000000;
    end_us = (int64_t)e * 1000000;

    while (keep_running) {
      set_position(conn, player, start_us);
      usleep((end_us - start_us) / 1000000 *
             1000000); // Wait for segment duration
    }
  } else if (argc == 5) {
    // loopctl part START END X - partial, X times
    int s = atoi(argv[2]);
    int e = atoi(argv[3]);
    int times = atoi(argv[4]);
    start_us = (int64_t)s * 1000000;
    end_us = (int64_t)e * 1000000;

    for (int loop = 0; loop < times && keep_running; loop++) {
      set_position(conn, player, start_us);
      usleep((end_us - start_us) / 1000000 *
             1000000); // Wait for segment duration
    }
  } else {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s                    # full, infinite\n", argv[0]);
    fprintf(stderr, "  %s N                  # full, N times\n", argv[0]);
    fprintf(stderr, "  %s part START END     # partial, infinite\n", argv[0]);
    fprintf(stderr, "  %s part START END N   # partial, N times\n", argv[0]);
    goto cleanup;
  }

cleanup:
  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);
  return EXIT_SUCCESS;
}
