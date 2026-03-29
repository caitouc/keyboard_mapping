#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <linux/input.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <signal.h>


volatile bool keep_running = true;

#define UINPUT_MAX_NAME_SIZE 80
struct uinput_user_dev {
    char name[UINPUT_MAX_NAME_SIZE];
    struct input_id id;
    unsigned int ff_effects_max;
    int absmax[64]; int absmin[64]; int absfuzz[64]; int absflat[64];
};
#define UI_SET_EVBIT    _IOW('U', 100, int)
#define UI_SET_KEYBIT   _IOW('U', 101, int)
#define UI_SET_RELBIT   _IOW('U', 102, int)
#define UI_SET_ABSBIT    _IOW('U', 103, int)
#define UI_SET_PROPBIT   _IOW('U', 110, int)
#define UI_DEV_CREATE   _IO('U', 1)
#define UI_DEV_DESTROY  _IO('U', 2)
#define UI_DEV_DETACH  _IO('U', 3)

#define ABS_MT_POSITION_X      0x35    /* Center X touch position */
#define ABS_MT_POSITION_Y      0x36    /* Center Y touch position */
#define ABS_MT_TOUCH_MAJOR      0x30    /* Center Y touch position */
#define ABS_MT_TRACKING_ID      0x39    
#define ABS_MT_PRESSURE      0x3a    /* Center Y touch position */

#define SYN_MT_REPORT 0x02

#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT  0x01
#define INPUT_PROP_POINTER  0x00
#endif

#ifndef REL_WHEEL
#define REL_WHEEL 0x08
#endif

// Global States
volatile int kb_fd = -1;
volatile int is_connected = 0;
pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;

int move_step = 3;
int shift_state = 0;
int alt_state = 0;    // Tracks KEY_LEFTALT, toggle key
int ctrl_state = 0;    // Tracks KEY_LEFTCTRL, glide key
int mouse_enabled = 0; // Global toggle
int touch_enabled = 0; // Global toggle
int debug_mode = 0;
int active_key = 0;

int curr_x = 0; // Start at center
int curr_y = 0;
int screen_w = 1280;
int screen_h = 720;
typedef enum{
    MODE_KEYBOARD,
    MODE_MOUSE,
    MODE_TOUCH
}InputMode;
InputMode input_mode = MODE_KEYBOARD;

void output(const char* format, ...) {
    if (debug_mode) {
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        fflush(stdout);
    }
}

void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type; ie.code = code; ie.value = val;
    write(fd, &ie, sizeof(ie));
}

char* find_k380_event() {
    FILE *fp = fopen("/proc/bus/input/devices", "r");
    if (!fp) return NULL;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Keyboard K380")) found = 1;
        if (found && strstr(line, "Handlers=")) {
            char *token = strstr(line, "event");
            if (token) {
                int num; 
                sscanf(token, "event%d", &num);
                char *event_node = malloc(32);
                sprintf(event_node, "/dev/input/event%d", num);
                fclose(fp); 
                return event_node;
            }
        }
        if (line[0] == '\n') found = 0;
    }
    fclose(fp); 
    return NULL;
}


// ... (keep your existing output, emit, and find_k380_event functions) ...

void* monitor_thread_func(void* arg) {
    char buf[4096];
    struct sockaddr_nl snl;

    // 1. Create Netlink socket to listen for Kernel Objects
    int nl_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (nl_fd < 0) return NULL;

    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    snl.nl_groups = 1; // Listen to group 1 (uevents)

    if (bind(nl_fd, (struct sockaddr *)&snl, sizeof(snl)) < 0) {
        close(nl_fd);
        return NULL;
    }

    // First-time scan to see if it's already plugged in
    char* initial_path = find_k380_event();
    if (initial_path) {
        pthread_mutex_lock(&fd_mutex);
        kb_fd = open(initial_path, O_RDONLY | O_NONBLOCK);
        if (kb_fd >= 0) is_connected = 1;
        pthread_mutex_unlock(&fd_mutex);
        free(initial_path);
    }

    while (keep_running) {
        // This blocks until a hardware change happens (0% CPU usage)
        ssize_t len = recv(nl_fd, buf, sizeof(buf) - 1, 0);
        //output("keyboard changed received, %s\n", buf);
        if (len <= 0) continue;
        buf[len] = '\0';

        // Check if the event is related to an input device and specifically the K380
        if (strstr(buf, "046D:B342") && strstr(buf, "event")) {
            if (strstr(buf, "add@")) {
                // Delay slightly to let /dev/input/eventX populate
                usleep(200000);
                char* path = find_k380_event();
                if (path) {
                    pthread_mutex_lock(&fd_mutex);
                    if (kb_fd >= 0) close(kb_fd);
                    kb_fd = open(path, O_RDONLY | O_NONBLOCK);
                    is_connected = (kb_fd >= 0);
                    output("keyboard changed: %d\n", is_connected);
                    pthread_mutex_unlock(&fd_mutex);
                    free(path);
                }
            }
            else if (strstr(buf, "remove@")) {
                pthread_mutex_lock(&fd_mutex);
                if (kb_fd >= 0) close(kb_fd);
                kb_fd = -1;
                is_connected = 0;
                output("keyboard changed: %d\n", is_connected);
                active_key = 0;
                pthread_mutex_unlock(&fd_mutex);
            }
        }
    }
    close(nl_fd);
    return NULL;
}

int setup_uinput_keyboard(int* p_uinput_fd){
    int uinput_fd = -1;
    struct uinput_user_dev uidev;
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) return 1;

    //set capabilities, ev_rel/rel_x/rel_y/btn_left -> draw mouse arrow(pointer)
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    int i;
    for (i = 0; i < 256; i++) ioctl(uinput_fd, UI_SET_KEYBIT, i);

    memset(&uidev, 0, sizeof(uidev));

    // Set VID/PID here
    uidev.id.bustype = BUS_USB;         // e.g., USB device
    uidev.id.vendor  = 0x046d;          // Logitech VID
    uidev.id.product = 0xb342;          // K380 PID
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "K380-Keyboard-Mode");

    write(uinput_fd, &uidev, sizeof(uidev));
    ioctl(uinput_fd, UI_DEV_CREATE);
    *p_uinput_fd = uinput_fd;
    return 0;
}

int setup_uinput_mouse(int* p_uinput_fd){
    int uinput_fd = -1;
    struct uinput_user_dev uidev;
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) return 1;

    //set capabilities, ev_rel/rel_x/rel_y/btn_left -> draw mouse arrow(pointer)
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_KEYBIT, KEY_LEFTCTRL);
    ioctl(uinput_fd, UI_SET_KEYBIT, KEY_RIGHTCTRL);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);

    ioctl(uinput_fd, UI_SET_EVBIT, EV_REL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_X);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_Y);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL); // Enable Scroll Wheel
    int i;
    for (i = 0; i < 256; i++) ioctl(uinput_fd, UI_SET_KEYBIT, i);

    memset(&uidev, 0, sizeof(uidev));

    // Set VID/PID here
    uidev.id.bustype = BUS_USB;         // e.g., USB device
    uidev.id.vendor  = 0x046d;          // Logitech VID
    uidev.id.product = 0xb342;          // K380 PID
    //uidev.id.version = 0x0110;          // Version (optional)
    //snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Keyboard K380");
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "K380-Mouse-Mode");

    write(uinput_fd, &uidev, sizeof(uidev));
    ioctl(uinput_fd, UI_DEV_CREATE);
    *p_uinput_fd = uinput_fd;
    return 0;
}

int setup_uinput_touch(int* p_uinput_fd){
    int uinput_fd = -1;
    struct uinput_user_dev uidev;
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) return 1;


    //set capabilities, ev_abs/abs_x/abs_y/btn_touch -> mInTouchMode=true, show touchpoint
    ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    // Enable Absolute Axes and Touch
    ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);   // Add this
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_Y);
                                       //
    //ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    //ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    //ioctl(uinput_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);   // Add this
    //ioctl(uinput_fd, UI_SET_ABSBIT, ABS_TOOL_WIDTH); // Add this

    // Also keep keyboard for passthrough
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    int i;
    for (i = 0; i < 256; i++) ioctl(uinput_fd, UI_SET_KEYBIT, i);

    memset(&uidev, 0, sizeof(uidev));

    // Set VID/PID here
    uidev.id.bustype = BUS_USB;         // e.g., USB device
    uidev.id.vendor  = 0x046d;          // Logitech VID
    uidev.id.product = 0xb342;          // K380 PID
    //uidev.id.version = 0x0110;          // Version (optional)
    //snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Keyboard K380");
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "K380-Touch-Mode");

    // Define the boundaries for X
    uidev.absmax[ABS_MT_POSITION_X] = 1280; // Matches your screen width
    uidev.absmax[ABS_MT_POSITION_Y] = 720; // Matches your screen height
    uidev.absmin[ABS_MT_POSITION_X] = 0; // Matches your screen width
    uidev.absmin[ABS_MT_POSITION_Y] = 0; // Matches your screen height
                                      //
    // Define screen resolution (Change to your TV's resolution)
    uidev.absmax[ABS_X] = 1280;
    uidev.absmax[ABS_Y] = 720;
    uidev.absmin[ABS_X] = 0;
    uidev.absmin[ABS_Y] = 0;
    //uidev.absmax[ABS_MT_TOUCH_MAJOR] = 255;
    //uidev.absmin[ABS_MT_TOUCH_MAJOR] = 0;
    uidev.absmax[ABS_MT_PRESSURE] = 255;
    uidev.absmin[ABS_MT_PRESSURE] = 0;
    //uidev.absmin[ABS_PRESSURE] = 0;
    //uidev.absmax[ABS_TOOL_WIDTH] = 15;
    //uidev.absmin[ABS_TOOL_WIDTH] = 0;

    // Tell the kernel where we are before we start moving
    write(uinput_fd, &uidev, sizeof(uidev));
    ioctl(uinput_fd, UI_DEV_CREATE);

    curr_x = 640;
    curr_y = 360;
    *p_uinput_fd = uinput_fd;
    return 0;
}

void cleanup_uinput(int* p_fd) {
    int fd = *p_fd;
    if (fd >= 0) {
        // 1. Send a Touch Up just in case the finger was "down"
        if(touch_enabled){
            emit(fd, EV_KEY, BTN_TOUCH, 0);
            emit(fd, EV_SYN, SYN_REPORT, 0);
        }

        // 2. Destroy the virtual device
        ioctl(fd, UI_DEV_DETACH, 0); // Some older kernels prefer this
        ioctl(fd, UI_DEV_DESTROY, 0);

        // 3. Close the file
        close(fd);
        output("Resources closed. Goodbye.\n");
    }
}

void process_event_keyboard(bool skip_toggle, struct input_event ev, int* p_uinput_fd){
    return;
}

void process_event_mouse(struct input_event ev, int* p_uinput_fd){
    int uinput_fd = *p_uinput_fd;

    if (ev.type == EV_KEY) {
        //if (!mouse_enabled) return;

        if (ev.value == 1 || ev.value == 2) {
            switch(ev.code) {
                case KEY_UP: 
                case KEY_DOWN: 
                    if(ctrl_state || alt_state){
                        active_key = ev.code; 
                    }
                case KEY_LEFT: 
                case KEY_RIGHT: 
                    if(ctrl_state){
                        active_key = ev.code; 
                    }
                    break;
                /*
                case KEY_PAGEUP:   
                    emit(uinput_fd, EV_REL, REL_WHEEL, 1); emit(uinput_fd, EV_SYN, SYN_REPORT, 0); break;
                case KEY_PAGEDOWN: 
                    emit(uinput_fd, EV_REL, REL_WHEEL, -1); emit(uinput_fd, EV_SYN, SYN_REPORT, 0); break;
                */
                case KEY_ENTER:    
                    if (ev.value == 1) { 
                        // 1. press button
                        emit(uinput_fd, EV_KEY, BTN_LEFT, 1); 
                        emit(uinput_fd, EV_SYN, SYN_REPORT, 0); 

                        // hold ctrl to enable jump on timeline on youtube
                        if(ctrl_state){
                            // 2. Micro-Move (1 pixel right, then 5 pixel left)
                            // This "activates" the scrubber in YouTube
                            emit(uinput_fd, EV_REL, REL_X, 1);
                            emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                            //usleep(1000);
                            emit(uinput_fd, EV_REL, REL_X, -4);
                            emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                        }
                    } 
                    return;
                case KEY_RIGHTALT: 
                    if (ev.value == 1) { emit(uinput_fd, EV_KEY, BTN_RIGHT, 1); emit(uinput_fd, EV_SYN, SYN_REPORT, 0); } break;
                case KEY_EQUAL:    
                    if (shift_state) { move_step++; output("Step: %d\n", move_step); } break;
                case KEY_MINUS:    
                    if (shift_state && move_step > 1) { move_step--; output("Step: %d\n", move_step); } break;
                    /*
                    if(shift_state && move_step >= 1){
                        active_key = ev.code; 
                    }
                    break;*/
            }
        } else if (ev.value == 0) {
            if (ev.code == active_key) {
                active_key = 0;
                return;
            }
            if (ev.code == KEY_ENTER) { 
                emit(uinput_fd, EV_KEY, BTN_LEFT, 0); 
                emit(uinput_fd, EV_SYN, SYN_REPORT, 0); 
                return;
            }
            if (ev.code == KEY_RIGHTALT) { 
                emit(uinput_fd, EV_KEY, BTN_RIGHT, 0); 
                emit(uinput_fd, EV_SYN, SYN_REPORT, 0); 
            }
        }
        output("pass to sys key: %d, %d\n", ev.code, ev.value);
        emit(*p_uinput_fd, EV_KEY, ev.code, ev.value);
        emit(*p_uinput_fd, EV_SYN, SYN_REPORT, 0);
    
    }
    //if (!mouse_enabled) continue;
}

void process_event_touch(struct input_event ev, int* p_uinput_fd){
    int uinput_fd = *p_uinput_fd;

    if (ev.type == EV_KEY) {
        /*
        if (!touch_enabled) {
            return;
        }*/

        switch(ev.code) {
            case KEY_UP: 
            case KEY_DOWN: 
            case KEY_LEFT: 
            case KEY_RIGHT: 
                if(ctrl_state){
                    if(ev.value == 1) {
                        active_key = ev.code; 
                    }else if(ev.value == 0) {
                        active_key = 0;
                    }
                    return;
                }
                break;
            case KEY_ENTER:    
                //output("key enter: %d\n", ev.value); 
                if(ev.value == 1){
                    // 1. Touch Down
                    emit(uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, 1);   // Start Finger #1
                    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, curr_x);
                    emit(uinput_fd, EV_ABS, ABS_MT_POSITION_Y, curr_y);
                    emit(uinput_fd, EV_ABS, ABS_MT_PRESSURE, 1);
                    emit(uinput_fd, EV_KEY, BTN_TOUCH, 1);
                    emit(uinput_fd, EV_SYN, SYN_MT_REPORT, 0);
                    emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                    
                    if(ctrl_state){
                        /*
                        output("key enter: %d\n", ev.value); 
                        // 2. Small "Macro Move" to break Touch Slop
                        usleep(10000); // 10ms
                        emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, curr_x + 10);
                        emit(uinput_fd, EV_SYN, SYN_MT_REPORT, 0);
                        emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                        usleep(10000); // 10ms
                        emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, curr_x - 20);
                        emit(uinput_fd, EV_SYN, SYN_MT_REPORT, 0);
                        emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                        */
                        return;
                    }
                }else if(ev.value == 0){
                    //emit(uinput_uinput_fd, EV_ABS, ABS_X, curr_x);
                    emit(uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);   // Kill Finger #1
                    emit(uinput_fd, EV_KEY, BTN_TOUCH, 0);
                    emit(uinput_fd, EV_SYN, SYN_MT_REPORT, 0);
                    emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
                    return;
                }
                break;
            case KEY_EQUAL:    
                if (shift_state) { 
                    move_step++; 
                    output("Step: %d\n", move_step); 
                    return;
                } 
                break;
            case KEY_MINUS:    
                if (shift_state && move_step > 1) { 
                    move_step--; 
                    output("Step: %d\n", move_step); 
                    return;
                } 
                break;
        }
        emit(uinput_fd, EV_KEY, ev.code, ev.value); 
        emit(uinput_fd, EV_SYN, SYN_REPORT, 0); 
    }
}

void do_glide_keyboard(int* p_uinput_fd){
    return;
}

void do_glide_mouse(int* p_uinput_fd){
    int uinput_fd = *p_uinput_fd;
    if(ctrl_state){
        output("glide to key: %d, move_step: %d\n", active_key, move_step);
        if (active_key == KEY_UP)    emit(uinput_fd, EV_REL, REL_Y, -move_step);
        if (active_key == KEY_DOWN)  emit(uinput_fd, EV_REL, REL_Y, move_step);
        if (active_key == KEY_LEFT)  emit(uinput_fd, EV_REL, REL_X, -move_step);
        if (active_key == KEY_RIGHT) emit(uinput_fd, EV_REL, REL_X, move_step);
        emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
    }
    if(alt_state){
        if (active_key == KEY_UP)    emit(uinput_fd, EV_REL, REL_WHEEL, 0x1);
        if (active_key == KEY_DOWN)  emit(uinput_fd, EV_REL, REL_WHEEL, 0xffffffff);
        emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
    }
    /*
    if(shift_state){
        if (active_key == KEY_EQUAL) {
            if (shift_state) { move_step++; output("Step: %d\n", move_step); };
        }
        if (active_key == KEY_MINUS) {
            if (shift_state && move_step > 1) { move_step--; output("Step: %d\n", move_step); };
        }
    }*/
}


void do_glide_touch(int* p_uinput_fd){
    int uinput_fd = *p_uinput_fd;
    if(ctrl_state){
        //output("glide...\n");
        if (active_key == KEY_UP)    curr_y -= move_step;
        if (active_key == KEY_DOWN)  curr_y += move_step;
        if (active_key == KEY_LEFT)  curr_x -= move_step;
        if (active_key == KEY_RIGHT) curr_x += move_step;
    
        // Boundary Checks
        if (curr_x < 0) curr_x = 0; if (curr_x > screen_w) curr_x = screen_w;
        if (curr_y < 0) curr_y = 0; if (curr_y > screen_h) curr_y = screen_h;
    
        // Emit the new position
        output("glide to key: %d, abs_x,abs_y: %d, %d\n", active_key, curr_x, curr_y);
        // Inside do_glide_touch when key is pressed:
        emit(uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, 1);   // Start Finger #1
        emit(uinput_fd, EV_ABS, ABS_MT_POSITION_X, curr_x);
        emit(uinput_fd, EV_ABS, ABS_MT_POSITION_Y, curr_y);
        //emit(uinput_fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
        
        emit(uinput_fd, EV_ABS, ABS_MT_PRESSURE, 1);
        emit(uinput_fd, EV_KEY, BTN_TOUCH, 1);
        //emit(uinput_fd, EV_KEY, BTN_TOOL_PEN, 1);
        
        // This is the signal for Protocol A (Android 4.4.2)
        emit(uinput_fd, EV_SYN, SYN_MT_REPORT, 0);
        emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
    }
}

// Signal Handler Function
void handle_sigint(int sig) {
    output("\nCaptured signal %d (Ctrl+C). Cleaning up...\n", sig);
    keep_running = false;
}

typedef void (*glide_fn_t)(int*);
void process_event_general(glide_fn_t* p_do_glide_func, struct input_event ev, int* p_uinput_fd){

    if (ev.type == EV_KEY) {
        // Track Modifiers
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_state = (ev.value > 0);
            return;
        }
        if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
            alt_state = (ev.value > 0);
            emit(*p_uinput_fd, EV_KEY, ev.code, ev.value);
            emit(*p_uinput_fd, EV_SYN, SYN_REPORT, 0);
            return;
        }

        if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
            ctrl_state = (ev.value > 0);
            emit(*p_uinput_fd, EV_KEY, ev.code, ev.value);
            emit(*p_uinput_fd, EV_SYN, SYN_REPORT, 0);
            //return;
        }

        // Toggle Logic: Alt + M, switch to mouse mode
        if (ev.code == KEY_M && alt_state && ev.value == 1) {
            mouse_enabled = !mouse_enabled;
            active_key = 0; // Reset movement on toggle
            cleanup_uinput(p_uinput_fd);
            if(mouse_enabled){
                touch_enabled = 0;
                setup_uinput_mouse(p_uinput_fd);
                ioctl(kb_fd, EVIOCGRAB, 1);
                *p_do_glide_func = do_glide_mouse;
            }else{
                ioctl(kb_fd, EVIOCGRAB, 0);
                setup_uinput_keyboard(p_uinput_fd);
                *p_do_glide_func = do_glide_keyboard;
            }
            output("Mouse Enabled: %d\n", mouse_enabled);
            return;
        }
        // Toggle Logic: Alt + T, switch to touch mode
        if (ev.code == KEY_T && alt_state && ev.value == 1) {
            touch_enabled = !touch_enabled;
            active_key = 0; // Reset movement on toggle
            cleanup_uinput(p_uinput_fd);
            if(touch_enabled){
                mouse_enabled = 0;
                setup_uinput_touch(p_uinput_fd);
                ioctl(kb_fd, EVIOCGRAB, 1);
                *p_do_glide_func = do_glide_touch;
            }else{
                ioctl(kb_fd, EVIOCGRAB, 0);
                setup_uinput_keyboard(p_uinput_fd);
                *p_do_glide_func = do_glide_keyboard;
            }
            output("Touch Enabled: %d\n", touch_enabled);
            return;
        }
        if (mouse_enabled){
            process_event_mouse(ev, p_uinput_fd);
            return;
        }
        if (touch_enabled) {
            process_event_touch(ev, p_uinput_fd);
            return;
        }
    }
}


int main(int argc, char **argv) {
    pthread_t monitor_tid;
    struct input_event ev;
    struct pollfd fds[1];
    int opt;
    bool log_once_disc = true;
    bool log_once_conn = true;
    signal(SIGINT, handle_sigint);

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') debug_mode= 1;
    }

    int (*setup_uinput_func)(int*) = NULL;
    void (*process_event_func)(glide_fn_t* func, struct input_event, int*) = NULL;
    void (*do_glide_func)(int*) = NULL;
    process_event_func = process_event_general;
    if(input_mode == MODE_KEYBOARD){
        output("mode used: keyboard mode\n");
        setup_uinput_func = setup_uinput_keyboard;
        do_glide_func = do_glide_keyboard;
    }else if(input_mode == MODE_MOUSE){
        output("mode used: mouse mode\n");
        setup_uinput_func = setup_uinput_mouse;
        do_glide_func = do_glide_mouse;
    }else if(input_mode == MODE_TOUCH){
        output("mode used: touch mode\n");
        setup_uinput_func = setup_uinput_touch;
        do_glide_func = do_glide_touch;
    }

    // 1. Setup uinput first (stays alive forever)
    //int uinput_fd = setup_uinput(); // Assume your setup_uinput func here
    int uinput_fd = -1;
    setup_uinput_func(&uinput_fd); // Assume your setup_uinput func here

    // 2. Start the Monitor Thread
    pthread_create(&monitor_tid, NULL, monitor_thread_func, NULL);

    while (keep_running) {
        int local_fd = -1;

        pthread_mutex_lock(&fd_mutex);
        if (is_connected) local_fd = kb_fd;
        pthread_mutex_unlock(&fd_mutex);

        if (local_fd < 0) {
            if(log_once_disc){
                output("keyboard disconnected \n");
                log_once_disc=false;
            }
            usleep(100000);
            continue;
        }else{
            log_once_disc=true;
            if(log_once_conn){
                output("keyboard connected \n");
                output("K380 Mouse v9 Active. Ctrl+M to toggle. Step: %d\n", move_step);
                log_once_conn=false;
            }
        }

        fds[0].fd = local_fd;
        fds[0].events = POLLIN;

        // 3. Main Logic Loop (Runs only when is_connected == 1)
        int ret = poll(fds, 1, 20);

        if (ret > 0) {
            pthread_mutex_lock(&fd_mutex);
            if(is_connected && kb_fd > 0){
                while (read(kb_fd, &ev, sizeof(struct input_event)) > 0) {
                    process_event_func(&do_glide_func, ev, &uinput_fd);
                }
            }
            pthread_mutex_unlock(&fd_mutex);
        }

        if ((touch_enabled || mouse_enabled) && active_key) {
            // Glide logic
            do_glide_func(&uinput_fd);
        }
    }
    cleanup_uinput(&uinput_fd);

    return 0;
}

