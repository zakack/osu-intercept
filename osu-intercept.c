#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

#define K1 KEY_Z //set the physical keys used
#define K2 KEY_X //to control the virtual keys
#define V1 K1 //v1 & v2 can be same as
#define V2 K2 //k1 & k2 or different

static inline int read_ev(struct input_event *ev) {
    return fread(ev, sizeof(struct input_event), 1, stdin) == 1;
}

static inline void write_ev(const struct input_event *ev) {
    if (fwrite(ev, sizeof(struct input_event), 1, stdout) != 1)
        exit(EXIT_FAILURE);
}

int main(void) {
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    struct input_event ie;
    struct input_event v1_up = {
    	  .type = EV_KEY,
    	  .code = V1,
    	  .value = 0
		};
    struct input_event v2_up = {
    	  .type = EV_KEY,
    	  .code = V2,
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
            ((ie.code != K1) && (ie.code != K2))) {
          	write_ev(&ie);
            continue;
        }
        if (ie.value == 2) ie.value = 1; //2 is key repeat
        if (ie.code == K1) k1 = ie.value;
        else k2 = ie.value;
        state = k1 + k2 + ie.value;
        switch (state) {
            case SINGLE: //key pressed alone
            act = (ie.code == K1) ? 1 : 0; //K1==V1 press
            ie.code = (ie.code == K1) ? V1 : V2;
            break;
            case RELEASE: //two keys released to one or
            case PRESS:   //one key pressed to two, toggle time
            write_ev(act ? &v1_up : &v2_up);
            write_ev(&syn); //emit release and flush buffer
            ie.value = 1; //hijack press event
            ie.code = act ? V2 : V1; //map using active flag
            act = !act; //toggle
            break;
            case NONE: //no keys pressed
            ie.code = act ? V1 : V2;
            act = 0;  //reset active flag
        }
    		write_ev(&ie); //pass ev to stdout
    }
    return 0;
}
