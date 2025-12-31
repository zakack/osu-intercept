#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

#define K1 KEY_Z //set the physical keys used
#define K2 KEY_X //to control the virtual keys

void print_usage(FILE *stream, const char *program) {
	  // clang-format off
    fprintf(stream,
            "osu-intercept - maps two keys onto two virtual keys\n"
            "\n"
            "usage: %s [-h | -z key1 | -x key2 | -Z virtual1 | -X virtual2]\n"
            "\n"
            "options:\n"
            "    -h show this message and exit\n"
            "    -z key code for first physical key (default: 'z')\n"
            "    -x key code for second physical key (default: 'x')\n"
            "    -Z key code for first virtual key (default: same as key1)\n"
            "    -X key code for second virtual key (default: same as key2)\n",
            program);
    // clang-format on
}

static inline int read_ev(struct input_event *ev) {
    return fread(ev, sizeof(struct input_event), 1, stdin) == 1;
}

static inline void write_ev(const struct input_event *ev) {
    if (fwrite(ev, sizeof(struct input_event), 1, stdout) != 1)
        exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	  int k1_code = K1;
	  int k2_code = K2;
	  int v1_code = 0;
	  int v2_code = 0;
    for (int opt; (opt = getopt(argc, argv, "hz:x:Z:X:")) != -1;) {
        switch (opt) {
            case 'h':
            return print_usage(stdout, argv[0]), EXIT_SUCCESS;
            case 'z':
            k1_code = atoi(optarg);
            continue;
            case 'x':
            k2_code = atoi(optarg);
            continue;
            case 'Z':
            v1_code = atoi(optarg);
            continue;
            case 'X':
            v2_code = atoi(optarg);
            continue;
        }

        return print_usage(stderr, argv[0]), EXIT_FAILURE;
    }
    if (!k1_code) k1_code = K1;
    if (!k2_code) k2_code = K2;
    if (!v1_code) v1_code = k1_code;
    if (!v2_code) v2_code = k2_code;
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    struct input_event ie;
    struct input_event v1_up = {
          .type = EV_KEY,
          .code = v1_code,
          .value = 0
        };
    struct input_event v2_up = {
          .type = EV_KEY,
          .code = v2_code,
          .value = 0
        };
    struct input_event syn = {
        .type = EV_SYN,
        .code = SYN_REPORT, 
        .value = 0
    };
    enum { NONE, RELEASE, SINGLE, PRESS } state = NONE;
    int k1 = 0; int k2 = 0;
    int act = 0;
    while (read_ev(&ie)) {
        if (ie.type == EV_MSC && ie.code == MSC_SCAN)
            continue;
        if ((ie.type != EV_KEY) || 
            ((ie.code != k1_code) && (ie.code != k2_code))) {
              write_ev(&ie);
            continue;
        }
        if (ie.value == 2) ie.value = 1; //2 is key repeat
        if (ie.code == k1_code) k1 = ie.value;
        else k2 = ie.value;
        state = k1 + k2 + ie.value;
        switch (state) {
            case SINGLE: //key pressed alone
            act = (ie.code == k1_code); //k1_code==v1_code press
            ie.code = act ? v1_code : v2_code;
            break;
            case RELEASE: //two keys released to one or
            case PRESS:   //one key pressed to two, toggle time
            write_ev(act ? &v1_up : &v2_up);
            write_ev(&syn); //emit release and flush buffer
            ie.value = 1; //hijack press event
            ie.code = act ? v2_code : v1_code; //map using active flag
            act = !act; //toggle
            break;
            case NONE: //no keys pressed
            ie.code = act ? v1_code : v2_code;
            act = 0;  //reset active flag
        }
            write_ev(&ie); //pass ev to stdout
    }
    return 0;
}
