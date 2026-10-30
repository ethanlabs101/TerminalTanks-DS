#include <string.h>
#include "content.h"

int design_by_key(const char *key) {
    for (int i = 0; i < N_DESIGNS; i++) if (!strcmp(DESIGNS[i].key, key)) return i;
    return -1;
}
int ammo_by_key(const char *key) {
    for (int i = 0; i < N_AMMOS; i++) if (!strcmp(AMMOS[i].key, key)) return i;
    return -1;
}
int map_by_key(const char *key) {
    for (int i = 0; i < N_MAPS_ALL; i++) if (!strcmp(MAPDEFS[i].key, key)) return i;
    return -1;
}
int uid_index(const char *uid) {
    for (int i = 0; i < N_UIDS; i++) if (!strcmp(UNLOCK_UIDS[i], uid)) return i;
    return -1;
}
int color_index_by_name(const char *name) {
    for (int i = 0; i < N_COLORS; i++) if (!strcmp(COLOR_CHOICES[i].name, name)) return i;
    return -1;
}
