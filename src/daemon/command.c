#include <limits.h>
#include "command.h"
#include "includes.h"
#include "device.h"
#include "devnode.h"
#include "led.h"
#include "notify.h"
#include "profile.h"
#include "usb.h"
#include <ckbnextconfig.h>

static const char* const cmd_strings[CMD_COUNT - 1] = {
    // NONE is implicit
    "delay",
    "mode",
    "switch",
    "layout",
    "accel",
    "scrollspeed",
    "notifyon",
    "notifyoff",
    "fps",
    "dither",

    "hwload",
    "hwsave",
    "fwupdate",
    "pollrate",

    "active",
    "idle",

    "erase",
    "eraseprofile",
    "name",
    "profilename",
    "id",
    "profileid",

    "rgb",
    "hwanim",
    "ioff",
    "ion",
    "iauto",

    "bind",
    "unbind",
    "rebind",
    "macro",

    "dpi",
    "dpisel",
    "lift",
    "snap",

    "notify",
    "inotify",
    "get",

    "reset"
};

#define TRY_WITH_RESET(action)  \
    while(action){              \
        if(usb_tryreset(kb)){   \
            return 1;           \
        }                       \
    }

#define HERTZ_LIM 16528925L // 60.5Hz

static inline long timespec_diff_ns (struct timespec* a, struct timespec* b){
    const time_t diff_s = a->tv_sec - b->tv_sec;
    const long diff_ns = a->tv_nsec - b->tv_nsec;
    // Check for possible overflow
    if(diff_ns > 0 && (LONG_MAX - diff_ns) / 1000000000L <= diff_s)
        return LONG_MAX;

    return diff_ns + diff_s * 1000000000L;
}

int readcmd(usbdevice* kb, char* line){
    const devcmd* vt = &kb->vtable;
    usbprofile* profile = kb->profile;
    usbmode* mode = profile->currentmode;
    int notifynumber = 0;
    // Read words from the input
    cmd command = NONE;
    char* ptr = NULL;
    char* intok = line;
    char* word = NULL;
    while((word = strtok_r(intok, " ", &ptr))){
        intok = NULL;
        // Check for a command word
        for(int i = 0; i < CMD_COUNT - 1; i++){
            if(!strcmp(word, cmd_strings[i])){
                command = i + CMD_FIRST;
#ifndef OS_MAC
                // Layout and mouse acceleration aren't used on Linux; ignore
                if(command == LAYOUT || command == ACCEL || command == SCROLLSPEED)
                    command = NONE;
#endif
                // Most commands require parameters, but a few are actions in and of themselves
                if(command != SWITCH
                        && command != HWLOAD && command != HWSAVE
                        && command != ACTIVE && command != IDLE
                        && command != ERASE && command != ERASEPROFILE)
                    goto next_loop;
                break;
            }
        }

        // Set current notification node when given @number
        int newnotify;
        if(sscanf(word, "@%d", &newnotify) == 1 && newnotify < OUTFIFO_MAX){
            notifynumber = newnotify;
            continue;
        }

        // Reject unrecognized commands. Reject bind or notify related commands if the keyboard doesn't have the feature enabled.
        if(command == NONE
                || ((!HAS_FEATURES(kb, FEAT_BIND) && (command == BIND || command == UNBIND || command == REBIND || command == MACRO || command == DELAY))
                           || (!HAS_FEATURES(kb, FEAT_NOTIFY) && command == NOTIFY))){
            next_loop:
            continue;
        }
        // Reject anything not related to fwupdate if device has a bricked FW
        if(NEEDS_FW_UPDATE(kb) && command != FWUPDATE && command != NOTIFYON && command != NOTIFYOFF && command != RESET)
            continue;

        // Specially handled commands - these are available even when keyboard is IDLE
        switch(command){
        case NOTIFYON: {
            // Notification node on
            int notify;
            if(sscanf(word, "%d", &notify) == 1)
                mknotifynode(kb, notify);
            continue;
        } case NOTIFYOFF: {
            // Notification node off
            int notify;
            if(sscanf(word, "%d", &notify) == 1 && notify != 0) // notify0 can't be removed
                rmnotifynode(kb, notify);
            continue;
        } case GET:
            // Output data to notification node
            vt->get(kb, mode, notifynumber, 0, word);
            continue;
        case LAYOUT:
            // OSX: switch ANSI/ISO keyboard layout
            if(!strcmp(word, "ansi"))
                kb->features = (kb->features & ~FEAT_LMASK) | FEAT_ANSI;
            else if(!strcmp(word, "iso"))
                kb->features = (kb->features & ~FEAT_LMASK) | FEAT_ISO;
            continue;
#ifdef OS_MAC_LEGACY
        case ACCEL:
            // OSX mouse acceleration on/off
            if(!strcmp(word, "on"))
                kb->features |= FEAT_MOUSEACCEL;
            else if(!strcmp(word, "off"))
                kb->features &= ~FEAT_MOUSEACCEL;
            continue;
        case SCROLLSPEED:{
            int newscroll;
            if(sscanf(word, "%d", &newscroll) != 1)
                break;
            if(newscroll < SCROLL_MIN)
                newscroll = SCROLL_ACCELERATED;
            if(newscroll > SCROLL_MAX)
                newscroll = SCROLL_MAX;
            kb->scroll_rate = newscroll;
            continue;
        }
#endif
        case MODE: {
            // Select a mode number (1 - 6)
            int newmode;
            if(sscanf(word, "%d", &newmode) == 1 && newmode > 0 && newmode <= MODE_COUNT)
                mode = profile->mode + newmode - 1;
            continue;
        }
        case FPS: {
            // USB command delay (2 - 10ms)
            uint framerate;
            if(sscanf(word, "%u", &framerate) == 1 && framerate > 0){
                // Not all devices require the same number of messages per frame; select delay appropriately
                uint per_frame = IS_MOUSE_DEV(kb) ? 2 : IS_FULLRANGE(kb) ? 14 : 5;
                uint delay = 1000 / framerate / per_frame;
                if(delay < 2)
                    delay = 2;
                else if(delay > 10)
                    delay = 10;
                kb->usbdelay = delay;
            }
            continue;
        }
        case DITHER: {
            // 0: No dither, 1: Ordered dither.
            uint dither;
            if(sscanf(word, "%u", &dither) == 1 && dither <= 1){
                kb->dither = dither;
                profile->currentmode->light.forceupdate = 1;
                mode->light.forceupdate = 1;
            }
            continue;
        }
        case DELAY: {
            continue;
        }
        case RESET: {
            vt->reset(kb, mode, notifynumber, 0, word);
            continue;
        }
        default:;
        }

        // If a keyboard is inactive, it must be activated before receiving any other commands
        if(!kb->active){
            if(command == ACTIVE)
                TRY_WITH_RESET(vt->active(kb, mode, notifynumber, 0, 0));
            continue;
        }
        // Specially handled commands only available when keyboard is ACTIVE
        switch(command){
        case IDLE:
            TRY_WITH_RESET(vt->idle(kb, mode, notifynumber, 0, 0));
            continue;
        case SWITCH:
            if(profile->currentmode != mode){
                queued_mutex_lock(imutex(kb));
                binding* bind = &profile->currentmode->bind;
                for (int i = 0; i < bind->macrocount; i++)
                    bind->macros[i].triggered = 0;
                profile->currentmode = mode;
                queued_mutex_unlock(imutex(kb));
                // Set mode light for non-RGB K95
                int index = INDEX_OF(mode, profile->mode);
                vt->setmodeindex(kb, index);
            }
            continue;
        case HWLOAD: case HWSAVE:{
            char delay = kb->usbdelay;
            // Ensure delay of at least 10ms as the device can get overwhelmed otherwise
            if(delay < 10)
                kb->usbdelay = 10;
            // Try to load/save the hardware profile. Reset on failure, disconnect if reset fails.
            TRY_WITH_RESET(vt->do_io[command](kb, mode, notifynumber, 1, 0));
            // Re-send the current RGB state as it sometimes gets scrambled
            TRY_WITH_RESET(vt->updatergb(kb, 1));
            kb->usbdelay = delay;
            continue;
        }
        case FWUPDATE:
            // FW update parses a whole word. Unlike hwload/hwsave, there's no try again on failure.
            if(vt->fwupdate(kb, mode, notifynumber, 0, word)){
                return 1;
            }
            continue;
        case POLLRATE: {
            if(HAS_FEATURES(kb, FEAT_ADJRATE)){
                pollrate_t rate;
                if(!strcmp(word, "8"))
                    rate = POLLRATE_8MS;
                else if(!strcmp(word, "4"))
                    rate = POLLRATE_4MS;
                else if(!strcmp(word, "2"))
                    rate = POLLRATE_2MS;
                else if(!strcmp(word, "1"))
                    rate = POLLRATE_1MS;
                else if(!strcmp(word, "0.5"))
                    rate = POLLRATE_05MS;
                else if(!strcmp(word, "0.25"))
                    rate = POLLRATE_025MS;
                else if(!strcmp(word, "0.1"))
                    rate = POLLRATE_01MS;
                else
                    continue;

                if(rate > kb->maxpollrate){
                    ckb_err("ckb%d: Poll rate %s ms is not supported by this device", INDEX_OF(kb, keyboard), word);
                    continue;
                }
                TRY_WITH_RESET(vt->pollrate(kb, rate));
            }
            continue;
        }
        case ERASEPROFILE:
            // Erase the current profile
            vt->eraseprofile(kb, mode, notifynumber, 0, 0);
            // Update profile/mode pointers
            profile = kb->profile;
            mode = profile->currentmode;
            continue;
        case ERASE: case NAME: case IOFF: case ION: case IAUTO: case INOTIFY: case PROFILENAME: case ID: case PROFILEID: case DPISEL: case LIFT: case SNAP:
            // All of the above just parse the whole word
            vt->do_cmd[command](kb, mode, notifynumber, 0, word);
            continue;
        case RGB: {
            // RGB command has a special response for a single hex constant
            uint r, g, b;
            if(sscanf(word, "%02x%02x%02x", &r, &g, &b) == 3){
                // Set all keys
                // We use -1 instead of notifynumber here to disable errors about duplicate led scancodes being set (in debug mode)
                // That parameter in cmd_rgb is a dummy anyway
                for(int i = 0; i < N_KEYS_EXTENDED; i++)
                    vt->rgb(kb, mode, -1, i, word);
                continue;
            }
            break;
        }
        case MACRO:
            if(!strcmp(word, "clear")){
                // Macro has a special clear command
                vt->macro(kb, mode, notifynumber, 0, 0);
                continue;
            }
            break;
        default:;
        }
        // For anything else, split the parameter at the colon
        int left = -1;
        sscanf(word, "%*[^:]%n", &left);
        if(left <= 0)
            continue;
        const char* right = word + left;
        if(right[0] == ':')
            right++;
        // Macros and DPI have a separate left-side handler
        if(command == MACRO || command == DPI){
            word[left] = 0;
            vt->do_macro[command](kb, mode, notifynumber, word, right);
            continue;
        }
        // Scan the left side for key names and run the requested command
        int position = 0, field = 0;
        char keyname[11] = { 0 };
        while(position < left && sscanf(word + position, "%10[^:,]%n", keyname, &field) == 1){
            uint keycode;
            if(!strcmp(keyname, "all")){
                // Set all keys
                for(int i = 0; i < N_KEYS_EXTENDED; i++)
                    vt->do_cmd[command](kb, mode, notifynumber, i, right);
            } else if((sscanf(keyname, "#%u", &keycode) && keycode < N_KEYS_EXTENDED)
                      || (sscanf(keyname, "#x%x", &keycode) && keycode < N_KEYS_EXTENDED)){
                // Set a key numerically
                vt->do_cmd[command](kb, mode, notifynumber, keycode, right);
            } else {
                // Find this key in the keymap
                for(unsigned i = 0; i < N_KEYS_EXTENDED; i++){
                    if(kb->keymap[i].name && !strcmp(keyname, kb->keymap[i].name)){
                        vt->do_cmd[command](kb, mode, notifynumber, i, right);
                        break;
                    }
                }
            }
            if(word[position += field] == ',')
                position++;
        }
    }

    // Finish up
    if(!NEEDS_FW_UPDATE(kb)){
        if(command == RGB){
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long int diff = timespec_diff_ns(&now, &kb->last_rgb);

            if(diff > 0 && diff < HERTZ_LIM){
#ifndef NDEBUG
                ckb_warn("ckb%d: RGB command called too quickly (%ld ns). Throttling...", INDEX_OF(kb, keyboard), diff);
#endif
                const long int sleep_ns = HERTZ_LIM - diff;
                clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec) {.tv_nsec = sleep_ns}, NULL);
                now.tv_nsec += sleep_ns;
#ifdef FPS_COUNTER
                diff += sleep_ns;
#endif
            }
#ifdef FPS_COUNTER
            ckb_info("ckb%d: FPS %f", INDEX_OF(kb, keyboard), 1.f / (diff / 1000000000.f));
#endif
            memcpy(&kb->last_rgb, &now, sizeof(struct timespec));
        }
        TRY_WITH_RESET(vt->updatergb(kb, 0));
        TRY_WITH_RESET(vt->updatedpi(kb, 0));
    }

#ifndef NDEBUG
    if(command == RGB)
        memset(kb->encounteredleds, 0, sizeof(kb->encounteredleds));
#endif

    return 0;
}
