#include "utils/utils.h"
#include <dbus/dbus.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;
static DBusConnection *conn;
static const char *player;
static int tfd; // single timerfd
static bool paused = false;
static uint64_t rem_us = 0; // usec remaining when paused

static void arm_timer(int tfd, uint64_t usec, bool periodic) {
  struct itimerspec its;
  its.it_value.tv_sec = usec / 1000000;
  its.it_value.tv_nsec = (usec % 1000000) * 1000;
  if (periodic) {
    its.it_interval = its.it_value;
  } else {
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
  }
  if (timerfd_settime(tfd, 0, &its, NULL) == -1) {
    perror("timerfd_settime");
    exit(1);
  }
}

void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
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

// DBus filter to catch PlaybackStatus changes
// DBusHandlerResult player_signal_filter(DBusConnection *conn, DBusMessage
// *msg,
//                                        void *usr) {
//   const char *iface;
//   DBusMessageIter top, changed, entry, variant;
//   if (!dbus_message_iter_init(msg, &top))
//     return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
//   if (dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_STRING)
//     return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
//   dbus_message_iter_get_basic(&top, &iface);
//   if (strcmp(iface, "org.mpris.MediaPlayer2.Player") != 0)
//     return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
//
//   // skip to the changed‑props dict
//   dbus_message_iter_next(&top);
//   dbus_message_iter_recurse(&top, &changed);
//
//   while (dbus_message_iter_get_arg_type(&changed) != DBUS_TYPE_INVALID) {
//     dbus_message_iter_recurse(&changed, &entry);
//     const char *key;
//     dbus_message_iter_get_basic(&entry, &key);
//     if (strcmp(key, "PlaybackStatus") == 0) {
//       dbus_message_iter_next(&entry);
//       dbus_message_iter_recurse(&entry, &variant);
//       const char *status;
//       dbus_message_iter_get_basic(&variant, &status);
//
//       if (strcmp(status, "Paused") == 0 && !paused) {
//         // freeze timer, record remaining
//         struct itimerspec cur;
//         if (timerfd_gettime(tfd, &cur) == -1) {
//           perror("timerfd_gettime");
//         } else {
//           rem_us = (uint64_t)cur.it_value.tv_sec * 1000000 +
//                    cur.it_value.tv_nsec / 1000;
//         }
//         // disarm
//         struct itimerspec dis = {{0, 0}, {0, 0}};
//         timerfd_settime(tfd, 0, &dis, NULL);
//         paused = true;
//
//       } else if (strcmp(status, "Playing") == 0 && paused) {
//         // restart one‑shot for leftover time
//         struct itimerspec go;
//         go.it_value.tv_sec = rem_us / 1000000;
//         go.it_value.tv_nsec = (rem_us % 1000000) * 1000;
//         go.it_interval.tv_sec = 0;
//         go.it_interval.tv_nsec = 0;
//         timerfd_settime(tfd, 0, &go, NULL);
//         paused = false;
//       }
//       break;
//     }
//     dbus_message_iter_next(&changed);
//   }
//   return DBUS_HANDLER_RESULT_HANDLED;
// }

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
  for (int i = 0; i < count; i++) {
    char *status = get_playback_status(conn, players[i]);
    if (status && strcmp(status, "Playing") == 0) {
      player = players[i];
      free(status);
      break;
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

  int64_t start_us, end_us, duration_us;
  int max_loops = -1; // infinite
  if (argc == 1) {
    start_us = 0;
    end_us = get_track_length(conn, player);
  } else if (argc == 2) {
    max_loops = atoi(argv[1]);
    start_us = 0;
    end_us = get_track_length(conn, player);
  } else if (argc == 4 && strcmp(argv[1], "-p") == 0) {
    start_us = (int64_t)atoi(argv[2]) * 1000000;
    end_us = (int64_t)atoi(argv[3]) * 1000000;
  } else if (argc == 5 && strcmp(argv[1], "-p") == 0) {
    max_loops = atoi(argv[4]);
    start_us = (int64_t)atoi(argv[2]) * 1000000;
    end_us = (int64_t)atoi(argv[3]) * 1000000;
  } else {
    usage();
  }
  duration_us = end_us - start_us;
  rem_us = duration_us;

  tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd == -1) {
    perror("timerfd_create");
    goto cleanup;
  }

  // subscribe to play/pause signals
  dbus_bus_add_match(conn,
                     "type='signal',"
                     "interface='org.freedesktop.DBus.Properties',"
                     "member='PropertiesChanged',"
                     "arg0='org.mpris.MediaPlayer2.Player'",
                     NULL);
  dbus_connection_flush(conn);

  set_position(conn, player, start_us);
  paused = false;
  struct itimerspec its = {.it_value = {.tv_sec = rem_us / 1000000,
                                        .tv_nsec = (rem_us % 1000000) * 1000},
                           .it_interval = {0, 0}};
  timerfd_settime(tfd, 0, &its, NULL);

  // prep for poll now :)
  dbus_int32_t dbus_fd;
  if (!dbus_connection_get_unix_fd(conn, &dbus_fd)) {
    fprintf(stderr, "Couldn't get DBus FD\n");
    exit(1);
  }
  struct pollfd pfd[2] = {
      {.fd = tfd, .events = POLLIN},
      {.fd = dbus_fd, .events = POLLIN},
  };

  int loops_done = 0;
  while (keep_running && (max_loops < 0 || loops_done < max_loops)) {
    int ret = poll(pfd, 2, -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    // DBus got something???? check for pause/play
    if (pfd[1].revents & POLLIN) {
      // drain and dispatch
      dbus_connection_read_write_dispatch(conn, 0);

      // then explicitly re‑query PlaybackStatus
      char *status = get_playback_status(conn, player);
      if (status) {
        if (strcmp(status, "Paused") == 0 && !paused) {
          // figure out exactly where we are
          int64_t pos = get_position(conn, player);
          if (pos < start_us)
            pos = start_us;
          if (pos > end_us)
            pos = end_us;
          rem_us = end_us - pos;

          // disarm timer
          struct itimerspec dis = {{0, 0}, {0, 0}};
          timerfd_settime(tfd, 0, &dis, NULL);
          paused = true;

        } else if (strcmp(status, "Playing") == 0 && paused) {
          // resume with the leftover time
          struct itimerspec go = {
              .it_value = {.tv_sec = rem_us / 1000000,
                           .tv_nsec = (rem_us % 1000000) * 1000},
              .it_interval = {0, 0}};
          timerfd_settime(tfd, 0, &go, NULL);
          paused = false;
        }
        free(status);
      }
    }

    // Timer expired???? only count if not paused
    if (!paused && (pfd[0].revents & POLLIN)) {
      uint64_t exp;
      if (read(tfd, &exp, sizeof(exp)) != sizeof(exp)) {
        perror("read timerfd");
        break;
      }
      loops_done++;
      set_position(conn, player, start_us);

      // next iteration full duration again
      rem_us = duration_us;
      struct itimerspec nxt = {
          .it_value = {.tv_sec = rem_us / 1000000,
                       .tv_nsec = (rem_us % 1000000) * 1000},
          .it_interval = {0, 0}};
      timerfd_settime(tfd, 0, &nxt, NULL);
    }
  }

  close(tfd);

cleanup:
  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);
  return EXIT_SUCCESS;
}
