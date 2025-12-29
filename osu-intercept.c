#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

#define K1 KEY_Z
#define K2 KEY_X

/* write key release event to stdout */
static inline void keyup(int code) {
    struct input_event ie = {0};
    ie.type = EV_KEY;
    ie.code = code;
    ie.value = 0;
    write(STDOUT_FILENO, &ie, sizeof(ie));
}

int main(void) {
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    struct input_event ie;
    struct input_event syn = {
        .type = EV_SYN,
        .code = SYN_REPORT, 
        .value = 0
    };
    enum State {
        NONE,
        RELEASE,
        SINGLE,
        PRESS
    };
    int k1 = 0; int k2 = 0;
    int act = 0;
    while (fread(&ie, sizeof(ie), 1, stdin) == 1) {
        //ignore EV_MSC events
        if (ie.type == EV_MSC && ie.code == MSC_SCAN)
            continue;
        //pass through non-key events and untracked keys
        if ((ie.type != EV_KEY) || 
            ((ie.code != K1) && (ie.code != K2))) {
            fwrite(&ie, sizeof(ie), 1, stdout);
            continue;
        }
        //track K1/K2
        if (ie.code == K1) k1 = ie.value;
        else k2 = ie.value;
        enum State state = k1 + k2 + ie.value;
        switch (state) {
            case RELEASE: //key pressed or released
            case PRESS:   //with one already pressed
            ie.value = 1;         //hijack press event
            keyup(act ? K1 : K2); //emit release
            ie.code = act ? K2 : K1;
            act = !act; //toggle
            fwrite(&ie, sizeof(ie), 1, stdout); //pass on event to stdout
            write(STDOUT_FILENO, &syn, sizeof(syn)); //flush buffer
            continue;
            case SINGLE: //key pressed alone
            if (ie.code == K2) break;
            act = !act; //set act flag if K1
            break;
            case NONE: //no keys pressed
            ie.code = act ? K1 : K2;
            act = 0;  //reset virtual state
        }
        fwrite(&ie, sizeof(ie), 1, stdout); //pass on event to stdout
    }
    return 0;
}
