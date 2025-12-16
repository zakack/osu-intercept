#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

#define GRAPE KEY_F13
#define BROCCOLI KEY_F14

// Direct inline emit function for maximum performance
static inline void emit(int fd, int type, int code, int val) {
    struct input_event ie = {0};
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
}

static inline void syn(int fd) {
    struct input_event ie = {0};
    ie.type = EV_SYN;
    ie.code = SYN_REPORT;
    ie.value = 0;
    write(fd, &ie, sizeof(ie));
}

// Optimized register/unregister with inline syn
static inline void register_code(int code) {
    emit(STDOUT_FILENO, EV_KEY, code, 1);
    syn(STDOUT_FILENO);
}

static inline void unregister_code(int code) {
    emit(STDOUT_FILENO, EV_KEY, code, 0);
    syn(STDOUT_FILENO);
}

int main(void) {
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    struct input_event ie;
    int key1_pressed = 0, key2_pressed = 0;
    int grape_down = 0, broccoli_down = 0;
    unsigned short last_key1 = 0, last_key2 = 0;

    while (fread(&ie, sizeof(ie), 1, stdin) == 1) {
        // Ignore EV_MSC events
        if (ie.type == EV_MSC && ie.code == MSC_SCAN)
            continue;
        // Ignore GRAPE or BROCCOLI events
        if (ie.code == GRAPE || ie.code == BROCCOLI)
            continue;
        // Pass through !EV_KEY events
        if (ie.type != EV_KEY) {
            fwrite(&ie, sizeof(ie), 1, stdout);
            continue;
        }

        // Handle key press events
        if (ie.value == 1) { // Key pressed
            if (ie.code == last_key1) {
                key1_pressed = 1;
            } else if (ie.code == last_key2) {
                key2_pressed = 1;
            } else {
                // New key: Shift and update states
                last_key1 = last_key2;
                last_key2 = ie.code;
                key1_pressed = key2_pressed;
                key2_pressed = 1;
            }
            if (key1_pressed && key2_pressed) {
                // Inline toggle logic for dual-key state
                if (grape_down) unregister_code(GRAPE);
                else register_code(GRAPE);
                grape_down = !grape_down;
                if (broccoli_down) unregister_code(BROCCOLI);
                else register_code(BROCCOLI);
                broccoli_down = !broccoli_down;
            } else {
                // Single key behavior (completed based on context)
                int new_grape = (ie.code == last_key1);
                int new_broccoli = !new_grape;
                if (grape_down != new_grape) {
                    if (grape_down) unregister_code(GRAPE);
                    else if (new_grape) register_code(GRAPE);
                    grape_down = new_grape;
                }
                if (broccoli_down != new_broccoli) {
                    if (broccoli_down) unregister_code(BROCCOLI);
                    else if (new_broccoli) register_code(BROCCOLI);
                    broccoli_down = new_broccoli;
                }
            }
        // Handle key release events
        } else if (ie.value == 0) {
            if (ie.code == last_key1) {
                key1_pressed = 0;
            } else if (ie.code == last_key2) {
                key2_pressed = 0;
            }
            if (key1_pressed || key2_pressed) {
                // Inline toggle for remaining key
                if (grape_down) unregister_code(GRAPE);
                else register_code(GRAPE);
                grape_down = !grape_down;
                if (broccoli_down) unregister_code(BROCCOLI);
                else register_code(BROCCOLI);
                broccoli_down = !broccoli_down;
            } else {
                // Both released: Reset to off
                if (grape_down) unregister_code(GRAPE);
                if (broccoli_down) unregister_code(BROCCOLI);
                grape_down = 0;
                broccoli_down = 0;
            }
            // Update pressed state
            if (ie.code == last_key2) {
                key2_pressed = 0;
            } else if (ie.code == last_key1) {
                key1_pressed = 0;
            }
        }
        // Pass through the original event
        fwrite(&ie, sizeof(ie), 1, stdout);
    }
    // Clean up on exit
    if (grape_down) unregister_code(GRAPE);
    if (broccoli_down) unregister_code(BROCCOLI);
    return 0;
}
