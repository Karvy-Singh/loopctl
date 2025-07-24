#ifndef UTILS_H
#define UTILS_H

#include <dbus/dbus.h>
#include <stdint.h>

DBusConnection *connect_session_bus(void);
char **list_mpris_names(DBusConnection *conn, int *out_count);
char *get_playback_status(DBusConnection *conn, const char *player);
dbus_int64_t get_position(DBusConnection *conn, const char *player);
void set_position(DBusConnection *conn, const char *player,
                  dbus_int64_t pos_us);
int64_t get_track_length(DBusConnection *conn, const char *bus_name);

#endif // !UTILS_H
