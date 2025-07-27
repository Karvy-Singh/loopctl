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
static const char *player; // the MPRIS player we’re looping

static void arm_timer(int tfd, uint64_t usec, bool periodic) {
  struct itimerspec its;
  // first expiration after 'usec' microseconds:
  its.it_value.tv_sec = usec / 1000000;
  its.it_value.tv_nsec = (usec % 1000000) * 1000;
  if (periodic) {
    // repeat every 'usec'
    its.it_interval = its.it_value;
  } else {
    // one-shot
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

int main(int argc, char *argv[]) {
  int tfd;
  int64_t start_us, end_us, duration_us;
  uint64_t expirations;
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

  tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (tfd == -1) {
    perror("timerfd_create");
    goto cleanup;
  }

  if (argc == 1) {
    // loopctl        -> full track, infinite
    start_us = 0;
    end_us = get_track_length(conn, player);
    duration_us = end_us - start_us;

    // first play immediately
    set_position(conn, player, start_us);
    // then arm a periodic timer
    arm_timer(tfd, duration_us, true);

    // every time it fires, reposition
    while (keep_running) {
      if (read(tfd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        perror("read timerfd");
        break;
      }
      set_position(conn, player, start_us);
    }

  } else if (argc == 2) {
    // loopctl X      -> full track, X times
    int times = atoi(argv[1]);
    start_us = 0;
    end_us = get_track_length(conn, player);
    duration_us = end_us - start_us;

    for (int loop = 0; loop < times && keep_running; loop++) {
      // reposition then wait one-shot
      set_position(conn, player, start_us);
      arm_timer(tfd, duration_us, false);
      if (read(tfd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        perror("read timerfd");
        break;
      }
    }

  } else if (argc == 4) {
    // loopctl part S E    -> partial segment, infinite
    int s = atoi(argv[2]), e = atoi(argv[3]);
    start_us = (int64_t)s * 1000000;
    end_us = (int64_t)e * 1000000;
    duration_us = end_us - start_us;

    set_position(conn, player, start_us);
    arm_timer(tfd, duration_us, true);

    while (keep_running) {
      if (read(tfd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        perror("read timerfd");
        break;
      }
      set_position(conn, player, start_us);
    }

  } else if (argc == 5) {
    // loopctl part S E X  -> partial segment, X times
    int s = atoi(argv[2]);
    int e = atoi(argv[3]);
    int times = atoi(argv[4]);
    start_us = (int64_t)s * 1000000;
    end_us = (int64_t)e * 1000000;
    duration_us = end_us - start_us;

    for (int loop = 0; loop < times && keep_running; loop++) {
      set_position(conn, player, start_us);
      arm_timer(tfd, duration_us, false);
      if (read(tfd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        perror("read timerfd");
        break;
      }
    }

  } else {
    usage();
    goto cleanup;
  }

  close(tfd);

cleanup:
  for (int i = 0; i < count; i++)
    free(players[i]);
  free(players);
  return EXIT_SUCCESS;
}
