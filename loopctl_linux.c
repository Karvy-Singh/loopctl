#include "utils/utils.h"
#include <dbus/dbus.h>
#include <errno.h>
#include <inttypes.h>
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
static const char *player;
static int timer_fd;
static int64_t start_us, end_us;
int loop_count = 0;
int max_loops = 0;

void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

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

static DBusHandlerResult filter_cb(DBusConnection *c, DBusMessage *m,
                                   void *userdata) {
  (void)c;
  (void)userdata;

  if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties",
                             "PropertiesChanged")) {
    const char *iface;
    DBusMessageIter iter, dict;
    dbus_message_iter_init(m, &iter);
    dbus_message_iter_get_basic(&iter, &iface);
    if (strcmp(iface, "org.mpris.MediaPlayer2.Player") == 0) {
      dbus_message_iter_next(&iter);
      dbus_message_iter_recurse(&iter, &dict);

      while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
        DBusMessageIter entry, variant;
        const char *prop;
        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &prop);

        if (strcmp(prop, "PlaybackStatus") == 0) {
          dbus_message_iter_next(&entry);
          dbus_message_iter_recurse(&entry, &variant);
          const char *status;
          dbus_message_iter_get_basic(&variant, &status);

          if (strcmp(status, "Playing") == 0) {
            int64_t pos = get_position(conn, player);
            arm_timer(pos);
          } else {
            /* pause: stop the timer */
            struct itimerspec off = {{0}, {0}};
            timerfd_settime(timer_fd, 0, &off, NULL);
          }
        }

        dbus_message_iter_next(&dict);
      }
    }

  } else if (dbus_message_is_signal(m, "org.mpris.MediaPlayer2.Player",
                                    "Seeked")) {
    int64_t new_pos;
    if (dbus_message_get_args(m, NULL, DBUS_TYPE_INT64, &new_pos,
                              DBUS_TYPE_INVALID)) {
      if (new_pos >= end_us) {
        set_position(conn, player, start_us);
      } else {
        arm_timer(new_pos);
      }
    }
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

void usage() {
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "  loopctl                    # full, infinite\n");
  fprintf(stderr, "  loopctl N                  # full, N times\n");
  fprintf(stderr, "  loopctl -p START END     # partial, infinite\n");
  fprintf(stderr, "  loopctl -p START END N   # partial, N times\n");
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_sigint);

  DBusError err;
  dbus_error_init(&err);
  conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (!conn) {
    fprintf(stderr, "Failed to connect to session bus: %s\n", err.message);
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

  if (argc == 1) {
    // loopctl - full, infinite
    start_us = 0;
    end_us = get_track_length(conn, player);
    set_position(conn, player, start_us);

  } else if (argc == 2) {
    // loopctl 5 - full, 5 times
    int times = atoi(argv[1]);
    start_us = 0;
    end_us = get_track_length(conn, player);
    max_loops = times;
    set_position(conn, player, start_us);
  } else if (argc == 4 || argc == 5) {
    if (strcmp(argv[1], "-p") == 0) {
      if (argc == 4) {
        // loopctl part START END - partial, infinite
        int s = atoi(argv[2]);
        int e = atoi(argv[3]);
        start_us = (int64_t)s * 1000000;
        end_us = (int64_t)e * 1000000;
        set_position(conn, player, start_us);

      } else if (argc == 5) {
        // loopctl part START END X - partial, X times
        int s = atoi(argv[2]);
        int e = atoi(argv[3]);
        int times = atoi(argv[4]);
        start_us = (int64_t)s * 1000000;
        end_us = (int64_t)e * 1000000;
        max_loops = times;
        set_position(conn, player, start_us);
      }
    } else {
      usage();
      goto cleanup;
    }

  } else {
    usage();
    goto cleanup;
  }

  dbus_bus_add_match(conn,
                     "type='signal',"
                     "interface='org.freedesktop.DBus.Properties',"
                     "member='PropertiesChanged',"
                     "arg0='org.mpris.MediaPlayer2.Player'",
                     NULL);
  dbus_bus_add_match(conn,
                     "type='signal',"
                     "interface='org.mpris.MediaPlayer2.Player',"
                     "member='Seeked'",
                     NULL);
  dbus_connection_add_filter(conn, filter_cb, NULL, NULL);

  timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (timer_fd < 0) {
    perror("timerfd_create");
    goto cleanup;
  }

  char *st = get_playback_status(conn, player);
  if (st && strcmp(st, "Playing") == 0) {
    int64_t pos = get_position(conn, player);
    arm_timer(pos);
  }
  free(st);

  int dbus_fd;
  if (!dbus_connection_get_unix_fd(conn, &dbus_fd)) {
    fprintf(stderr, "Failed to get D-Bus unix fd\n");
    close(timer_fd);
    goto cleanup;
  }

  struct pollfd pfds[2];
  while (keep_running) {
    pfds[0].fd = dbus_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = timer_fd;
    pfds[1].events = POLLIN;

    if (poll(pfds, 2, -1) < 0 && errno != EINTR) {
      perror("poll");
      break;
    }

    if (pfds[0].revents & POLLIN)
      dbus_connection_read_write_dispatch(conn, 0);

    if (pfds[1].revents & POLLIN) {
      uint64_t expirations = 0;
      if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {

        if (expirations == 0)
          expirations = 1;

        loop_count += (int)expirations;
        fprintf(stderr, "[loopctl] segment done (%d/%d)\n", loop_count,
                max_loops);

        if (max_loops > 0 && loop_count >= max_loops) {
          fprintf(stderr, "[loopctl] reached max loops, exiting\n");
          keep_running = 0;
          continue;
        }

        set_position(conn, player, start_us);
        arm_timer(start_us);
        fprintf(stderr, "[loopctl] rewound to %" PRId64 " µs\n", start_us);
      }
    }
  }

  close(timer_fd);
  dbus_connection_remove_filter(conn, filter_cb, NULL);

cleanup:
  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);

  return EXIT_SUCCESS;
}
