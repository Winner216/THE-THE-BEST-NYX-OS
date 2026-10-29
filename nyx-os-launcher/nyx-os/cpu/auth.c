// cpu/auth.c
#include <auth.h>

User users[MAX_USERS];
int  user_count    = 0;
int  logged_in_user = 0;

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void auth_init() {
    // Create default admin user
    str_copy(users[0].username, "admin", MAX_USERNAME);
    str_copy(users[0].password, "1234",  MAX_PASSWORD);
    users[0].active = 1;
    user_count = 1;
    logged_in_user = -1;
}

int auth_login(const char* username, const char* password) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active &&
            str_eq(users[i].username, username) &&
            str_eq(users[i].password, password)) {
            logged_in_user = i;
            return 1;
        }
    }
    return 0;
}

int auth_add_user(const char* username, const char* password) {
    if (user_count >= MAX_USERS) return 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) {
            str_copy(users[i].username, username, MAX_USERNAME);
            str_copy(users[i].password, password, MAX_PASSWORD);
            users[i].active = 1;
            user_count++;
            return 1;
        }
    }
    return 0;
}

int auth_change_password(int user_idx, const char* old_pass, const char* new_pass) {
    if (user_idx < 0 || user_idx >= MAX_USERS) return 0;
    if (!users[user_idx].active) return 0;
    if (!str_eq(users[user_idx].password, old_pass)) return 0;
    str_copy(users[user_idx].password, new_pass, MAX_PASSWORD);
    return 1;
}

int auth_delete_user(int user_idx) {
    if (user_idx < 0 || user_idx >= MAX_USERS) return 0;
    if (user_idx == 0) return 0; // Can't delete admin
    if (user_idx == logged_in_user) return 0;
    users[user_idx].active = 0;
    user_count--;
    return 1;
}
