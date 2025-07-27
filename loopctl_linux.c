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
static const char *player; // the MPRIS player we’re looping
static int tfd;            // our single timerfd
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

// Catch Ctrl‑C
void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

// Print usage and exit
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
DBusHandlerResult player_signal_filter(DBusConnection *conn, DBusMessage *msg,
                                       void *usr) {
  const char *iface;
  DBusMessageIter top, changed, entry, variant;
  if (!dbus_message_iter_init(msg, &top))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  if (dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_STRING)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  dbus_message_iter_get_basic(&top, &iface);
  if (strcmp(iface, "org.mpris.MediaPlayer2.Player") != 0)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  // skip to the changed‑props dict
  dbus_message_iter_next(&top);
  dbus_message_iter_recurse(&top, &changed);

  while (dbus_message_iter_get_arg_type(&changed) != DBUS_TYPE_INVALID) {
    dbus_message_iter_recurse(&changed, &entry);
    const char *key;
    dbus_message_iter_get_basic(&entry, &key);
    if (strcmp(key, "PlaybackStatus") == 0) {
      dbus_message_iter_next(&entry);
      dbus_message_iter_recurse(&entry, &variant);
      const char *status;
      dbus_message_iter_get_basic(&variant, &status);

      if (strcmp(status, "Paused") == 0 && !paused) {
        // freeze timer, record remaining
        struct itimerspec cur;
        if (timerfd_gettime(tfd, &cur) == -1) {
          perror("timerfd_gettime");
        } else {
          rem_us = (uint64_t)cur.it_value.tv_sec * 1000000 +
                   cur.it_value.tv_nsec / 1000;
        }
        // disarm
        struct itimerspec dis = {{0, 0}, {0, 0}};
        timerfd_settime(tfd, 0, &dis, NULL);
        paused = true;

      } else if (strcmp(status, "Playing") == 0 && paused) {
        // restart one‑shot for leftover time
        struct itimerspec go;
        go.it_value.tv_sec = rem_us / 1000000;
        go.it_value.tv_nsec = (rem_us % 1000000) * 1000;
        go.it_interval.tv_sec = 0;
        go.it_interval.tv_nsec = 0;
        timerfd_settime(tfd, 0, &go, NULL);
        paused = false;
      }
      break;
    }
    dbus_message_iter_next(&changed);
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_sigint);

  // 1) connect to session bus
  conn = connect_session_bus();
  if (!conn) {
    fprintf(stderr, "Failed to connect to session bus\n");
    return EXIT_FAILURE;
  }

  // 2) find a playing player
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

  // 3) subscribe to play/pause signals
  dbus_connection_add_filter(conn, player_signal_filter, NULL, NULL);
  dbus_bus_add_match(conn,
                     "type='signal',"
                     "interface='org.freedesktop.DBus.Properties',"
                     "member='PropertiesChanged',"
                     "arg0='org.mpris.MediaPlayer2.Player'",
                     NULL);

  // 4) determine loop parameters
  int64_t start_us, end_us, duration_us;
  int max_loops = -1; // -1 means infinite
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

  // 5) create the timerfd
  tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd == -1) {
    perror("timerfd_create");
    goto cleanup;
  }

  // 6) prep for poll()
  dbus_int32_t dbus_fd;
  if (!dbus_connection_get_unix_fd(conn, &dbus_fd)) {
    fprintf(stderr, "Failed to retrieve D‑Bus file descriptor\n");
    close(tfd);
    goto cleanup;
  }
  struct pollfd pfd[2] = {
      {.fd = tfd, .events = POLLIN},
      {.fd = dbus_fd, .events = POLLIN},
  };

  // 7) start the first play + timer
  set_position(conn, player, start_us);
  paused = false;
  rem_us = duration_us;
  arm_timer(tfd, rem_us, false);

  // 8) main loop
  int loops_done = 0;
  while (keep_running && (max_loops < 0 || loops_done < max_loops)) {
    int ret = poll(pfd, 2, -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    // 8a) D‑Bus events (pause/play)
    if (pfd[1].revents & POLLIN) {
      dbus_connection_read_write_dispatch(conn, 0);
    }

    // 8b) timer fired (and not paused)
    if (!paused && (pfd[0].revents & POLLIN)) {
      uint64_t expirations;
      if (read(tfd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        perror("read timerfd");
        break;
      }
      loops_done++;
      set_position(conn, player, start_us);

      // reset for next iteration
      rem_us = duration_us;
      arm_timer(tfd, rem_us, false);
    }
  }

  close(tfd);

cleanup:
  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);
  return EXIT_SUCCESS;
}
