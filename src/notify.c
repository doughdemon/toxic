/*  notify.c
 * 
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "notify.h"
#include "device.h"
#include "settings.h"
#include "line_info.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include <sys/stat.h>

#if defined(_AUDIO) || defined(_SOUND_NOTIFY)
    #ifdef __APPLE__
        #include <OpenAL/al.h>
        #include <OpenAL/alc.h>
    #else
        #include <AL/al.h>
        #include <AL/alc.h>
        /* compatibility with older versions of OpenAL */
        #ifndef ALC_ALL_DEVICES_SPECIFIER
            #include <AL/alext.h>
        #endif
    #endif
    #ifdef _SOUND_NOTIFY
        #include <AL/alut.h> /* freealut packet */
    #endif
#endif /* _AUDIO */

#ifdef _X11
    #include <X11/Xlib.h>
#endif /* _X11 */

#ifdef _BOX_NOTIFY
    #include <libnotify/notify.h>
#endif

#define SOUNDS_SIZE 10
#define ACTIVE_NOTIFS_MAX 50

extern struct user_settings *user_settings_;

struct _Control {
    time_t cooldown;
    time_t notif_timeout;
    unsigned long this_window;
#ifdef _X11
    Display *display;
#endif /* _X11 */
    
#if defined(_SOUND_NOTIFY) || defined(_BOX_NOTIFY)
    pthread_mutex_t poll_mutex[1];
    _Bool poll_active;
#endif
    
#ifdef _SOUND_NOTIFY
    uint32_t device_idx; /* index of output device */
    char* sounds[SOUNDS_SIZE];
#endif /* _SOUND_NOTIFY */
} Control = {0};

struct _ActiveNotifications {
#ifdef _SOUND_NOTIFY
    uint32_t source;
    uint32_t buffer;
    _Bool looping;
#endif
    _Bool active;
    int *id_indicator;
#ifdef _BOX_NOTIFY
    NotifyNotification* box;
    char messages[128][128];
    char title[24];
    size_t size;
    time_t n_timeout;
#endif
} actives[ACTIVE_NOTIFS_MAX] = {{0}};
/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/

long unsigned int get_focused_window_id()
{
#ifdef _X11
    if (!Control.display) return 0;
    
    Window focus;
    int revert;
    XGetInputFocus(Control.display, &focus, &revert);
    return focus;
#else
    return 0;
#endif /* _X11 */
}

static void control_lock()
{
#if defined(_SOUND_NOTIFY) || defined(_BOX_NOTIFY)
    pthread_mutex_lock(Control.poll_mutex);
#endif
}

static void control_unlock()
{
#if defined(_SOUND_NOTIFY) || defined(_BOX_NOTIFY)
    pthread_mutex_unlock(Control.poll_mutex);
#endif
}

#ifdef _SOUND_NOTIFY
_Bool is_playing(int source)
{
    int ready;
    alGetSourcei(source, AL_SOURCE_STATE, &ready);
    return ready == AL_PLAYING;
}

/* Terminate all sounds but wait them to finish first */
void graceful_clear()
{
    int i;
    control_lock();
    while (1) {
        for (i = 0; i < ACTIVE_NOTIFS_MAX; i ++) {
            if (actives[i].active) {
            #ifdef _BOX_NOTIFY
                if (actives[i].box) {
                    GError* ignore;
                    notify_notification_close(actives[i].box, &ignore);
                    actives[i].box = NULL;
                }
            #endif
            
                if(actives[i].id_indicator) *actives[i].id_indicator = -1; // reset indicator value
                
                if ( actives[i].looping ) {
                    stop_sound(i); 
                } else {
                    if (!is_playing(actives[i].source)) 
                        memset(&actives[i], 0, sizeof(struct _ActiveNotifications));
                    else break;
                }
            }
            
        }
        
        if (i == ACTIVE_NOTIFS_MAX) {
            control_unlock();
            return;
        }
            
        usleep(1000);
    }
}

void* do_playing(void* _p)
{
    (void)_p;
    int i;
    while(Control.poll_active) {
        control_lock();
        for (i = 0; i < ACTIVE_NOTIFS_MAX; i ++) {
            if (actives[i].active && !actives[i].looping
                #ifdef _BOX_NOTIFY
                    && !actives[i].box
                #endif
            ) {
                if (!is_playing(actives[i].source)) {
                /* Close */                    
                    alSourceStop(actives[i].source);
                    alDeleteSources(1, &actives[i].source);
                    alDeleteBuffers(1,&actives[i].buffer);
                    memset(&actives[i], 0, sizeof(struct _ActiveNotifications));
                }
            }
        #ifdef _BOX_NOTIFY
            else if (actives[i].box && time(NULL) >= actives[i].n_timeout)
            {
                GError* ignore;
                notify_notification_close(actives[i].box, &ignore);
                actives[i].box = NULL;
                
                if(actives[i].id_indicator) *actives[i].id_indicator = -1; // reset indicator value

                if (!actives[i].looping && !is_playing(actives[i].source)) {
                /* stop source if not looping or playing, just terminate box */
                    alSourceStop(actives[i].source);
                    alDeleteSources(1, &actives[i].source);
                    alDeleteBuffers(1,&actives[i].buffer);
                    memset(&actives[i], 0, sizeof(struct _ActiveNotifications));
                }
            }
        #endif
        }
        control_unlock();
        usleep(10000);
    }
    pthread_exit(NULL);
}

int play_source(uint32_t source, uint32_t buffer, _Bool looping)
{    
    int i = 0;
    for (; i < ACTIVE_NOTIFS_MAX && actives[i].active; i ++);
    if ( i == ACTIVE_NOTIFS_MAX ) {
        return -1; /* Full */
    }
    
    alSourcePlay(source);
    
    actives[i].active = 1;
    actives[i].source = source;
    actives[i].buffer = buffer;
    actives[i].looping = looping;
    
    return i;
}

#elif _BOX_NOTIFY
void* do_playing(void* _p)
{
    (void)_p;
    int i;
    while(Control.poll_active) {
        control_lock();
        for (i = 0; i < ACTIVE_NOTIFS_MAX; i ++) {
            if (actives[i].box && time(NULL) >= actives[i].n_timeout)
            {
                GError* ignore;
                notify_notification_close(actives[i].box, &ignore);
                actives[i].box = NULL;
                
                if(actives[i].id_indicator) *actives[i].id_indicator = -1; // reset indicator value
                memset(&actives[i], 0, sizeof(struct _ActiveNotifications));
            }
        }
        control_unlock();
        usleep(10000);
    }
    pthread_exit(NULL);
}

void graceful_clear()
{
    int i;
    control_lock();
    
    for (i = 0; i < ACTIVE_NOTIFS_MAX; i ++) {
        if (actives[i].box) {
            GError* ignore;
            notify_notification_close(actives[i].box, &ignore);
            actives[i].box = NULL;
        }
        
        if(actives[i].id_indicator) *actives[i].id_indicator = -1; // reset indicator value
        memset(&actives[i], 0, sizeof(struct _ActiveNotifications));
    }   
    
    control_unlock();
}
#endif

/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/
/**********************************************************************************/



/* Opens primary device */
int init_notify(int login_cooldown, int notification_timeout)
{
#ifdef _SOUND_NOTIFY
    alutInitWithoutContext(NULL, NULL);
    if (open_primary_device(output, &Control.device_idx, 48000, 20, 1) != de_None)
        return -1;
#endif /* _SOUND_NOTIFY */
        
#if defined(_SOUND_NOTIFY) || defined(_BOX_NOTIFY)
    pthread_mutex_init(Control.poll_mutex, NULL);
    pthread_t thread;
    if (pthread_create(&thread, NULL, do_playing, NULL) != 0 || pthread_detach(thread) != 0 ) {
        pthread_mutex_destroy(Control.poll_mutex);
        return -1;
    }
    Control.poll_active = 1;
#endif
    
    Control.cooldown = time(NULL) + login_cooldown;
#ifdef _X11
    Control.display = XOpenDisplay(NULL);
    Control.this_window = get_focused_window_id();
#else
    Control.this_window = 1;
#endif /* _X11 */
    
    
#ifdef _BOX_NOTIFY
    notify_init("toxic");
#endif
    Control.notif_timeout = notification_timeout;
    return 1;
}

void terminate_notify()
{    
#if defined(_SOUND_NOTIFY) || defined(_BOX_NOTIFY)    
    if ( !Control.poll_active ) return;    
    Control.poll_active = 0;
    
    graceful_clear();
#endif

#ifdef _SOUND_NOTIFY
    int i = 0;
    for (; i < SOUNDS_SIZE; i ++) free(Control.sounds[i]);
    close_device(output, Control.device_idx);
    alutExit();
#endif /* _SOUND_NOTIFY */
    
#ifdef _BOX_NOTIFY
    notify_uninit();
#endif
}

#ifdef _SOUND_NOTIFY
int set_sound(Notification sound, const char* value)
{
    if (sound == silent) return 0;
    
    free(Control.sounds[sound]);

    size_t len = strlen(value) + 1;
    Control.sounds[sound] = calloc(len, 1);
    memcpy(Control.sounds[sound], value, len);

    struct stat buf;
    return stat(value, &buf) == 0;
}

int play_sound_internal(Notification what, _Bool loop)
{        
    uint32_t source;
    uint32_t buffer;
    
    alGenSources(1, &source);
    alGenBuffers(1, &buffer);
    buffer = alutCreateBufferFromFile(Control.sounds[what]);
    alSourcei(source, AL_BUFFER, buffer);
    alSourcei(source, AL_LOOPING, loop);
    
    int rc = play_source(source, buffer, loop);
    if (rc < 0) {
        alSourceStop(source);
        alDeleteSources(1, &source);
        alDeleteBuffers(1,&buffer);
        return -1;
    }
    
    return rc;
}

int play_notify_sound(Notification notif, uint64_t flags)
{
    int rc = -1;

    if (flags & NT_BEEP) beep();
    else if (notif != silent) {
        if ( !Control.poll_active || !Control.sounds[notif] )
            return -1;

        rc = play_sound_internal(notif, flags & NT_LOOP ? 1 : 0);
    }

    return rc;
}


void stop_sound(int id)
{
    if (id >= 0 && id < ACTIVE_NOTIFS_MAX && actives[id].looping && actives[id].active ) {
#ifdef _BOX_NOTIFY
        if (actives[id].box) {
            GError* ignore;
            notify_notification_close(actives[id].box, &ignore);
        }
#endif
        *actives[id].id_indicator = -1;
//         alSourcei(actives[id].source, AL_LOOPING, false);
        alSourceStop(actives[id].source);
        alDeleteSources(1, &actives[id].source);
        alDeleteBuffers(1,&actives[id].buffer);
        memset(&actives[id], 0, sizeof(struct _ActiveNotifications));
    }
}
#endif

static int m_play_sound(Notification notif, uint64_t flags)
{
#ifdef _SOUND_NOTIFY
    return play_notify_sound(notif, flags);
#else
    if (notif != silent)
        beep();

    return -1;
#endif /* _SOUND_NOTIFY */   
}

#ifdef _BOX_NOTIFY
void m_notify_action(NotifyNotification *box, char *action, void* data) 
{
}
#endif

int sound_notify(ToxWindow* self, Notification notif, uint64_t flags, int* id_indicator)
{
    /* Consider colored notify as primary */
    if (self && self->alert == WINDOW_ALERT_NONE) {
        if (flags & NT_WNDALERT_0) self->alert = WINDOW_ALERT_0;
        else if (flags & NT_WNDALERT_1) self->alert = WINDOW_ALERT_1;
        else if (flags & NT_WNDALERT_2) self->alert = WINDOW_ALERT_2;
    }
    
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
    
    int id = -1;
    control_lock();
    
    if (self && (!self->stb || self->stb->status != TOX_USERSTATUS_BUSY) && user_settings_->alerts == ALERTS_ENABLED) 
        id = m_play_sound(notif, flags);
    
    else if (flags & NT_ALWAYS)
        id = m_play_sound(notif, flags);
    
#if defined(_BOX_NOTIFY) && !defined(_SOUND_NOTIFY)

    if (id == -1) {
        for (id = 0; id < ACTIVE_NOTIFS_MAX && actives[id].box; id ++);
        if ( id == ACTIVE_NOTIFS_MAX ) {
            control_unlock();
            return -1; /* Full */
        }
    }
    
#endif
    
    if ( id_indicator && id != -1 ) { 
        actives[id].id_indicator = id_indicator;
        *id_indicator = id;
    }
    
    control_unlock();
    
    return id;
}

int sound_notify2(ToxWindow* self, Notification notif, uint64_t flags, int id)
{
    /* Consider colored notify as primary */
    if (self && self->alert == WINDOW_ALERT_NONE) {
        if (flags & NT_WNDALERT_0) self->alert = WINDOW_ALERT_0;
        else if (flags & NT_WNDALERT_1) self->alert = WINDOW_ALERT_1;
        else if (flags & NT_WNDALERT_2) self->alert = WINDOW_ALERT_2;
    }
    
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
    
    if (id < 0 || id >= ACTIVE_NOTIFS_MAX) return -1;
#ifdef _SOUND_NOTIFY    
    control_lock();
    
    if (!actives[id].active || !Control.sounds[notif]) {
        control_unlock();
        return -1;
    }
    
    alSourceStop(actives[id].source);
    alDeleteSources(1, &actives[id].source);
    alDeleteBuffers(1,&actives[id].buffer);
    
    
    alGenSources(1, &actives[id].source);
    alGenBuffers(1, &actives[id].buffer);
    actives[id].buffer = alutCreateBufferFromFile(Control.sounds[notif]);
    alSourcei(actives[id].source, AL_BUFFER, actives[id].buffer);
    alSourcei(actives[id].source, AL_LOOPING, flags & NT_LOOP);
    
    alSourcePlay(actives[id].source);
    
    control_unlock();
    
    return id;
#else
    if (notif != silent)
        beep();
    
    return 0;
#endif /* _SOUND_NOTIFY */
}

int box_notify(ToxWindow* self, Notification notif, uint64_t flags, int* id_indicator, char* title, const char* format, ...)
{
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
        
#ifdef _BOX_NOTIFY
    
    int id = sound_notify(self, notif, flags, id_indicator);
    
    control_lock();
    
#ifdef _SOUND_NOTIFY
    if (id == -1) { /* Could not play */
        
        for (id = 0; id < ACTIVE_NOTIFS_MAX && actives[id].active; id ++);
        if ( id == ACTIVE_NOTIFS_MAX ) {
            control_unlock();
            return -1; /* Full */
        }
        
        actives[id].active = 1;    
        actives[id].id_indicator = id_indicator;
        if (id_indicator) *id_indicator = id;
    }
#endif
        
    strncpy(actives[id].title, title, 24);
    if (strlen(title) > 23) strcpy(actives[id].title + 20, "...");

    va_list __ARGS__; va_start (__ARGS__, format);
    vsnprintf (actives[id].messages[0], 127, format, __ARGS__);
    va_end (__ARGS__);
    
    if (strlen(actives[id].messages[0]) > 124) 
        strcpy(actives[id].messages[0] + 124, "...");
    
    actives[id].box = notify_notification_new(actives[id].title, actives[id].messages[0], NULL);
    actives[id].size ++;
    actives[id].n_timeout = time(NULL) + Control.notif_timeout / 1000;
    
    notify_notification_set_timeout(actives[id].box, Control.notif_timeout);
    notify_notification_set_app_name(actives[id].box, "toxic");
    /*notify_notification_add_action(actives[id].box, "lel", "default", m_notify_action, self, NULL);*/
    notify_notification_show(actives[id].box, NULL);
    
    control_unlock();
    return id;
#else
    return sound_notify(self, notif, flags, id_indicator);
#endif
}

int box_notify2(ToxWindow* self, Notification notif, uint64_t flags, int id, const char* format, ...)
{
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
        
#ifdef _BOX_NOTIFY
    
    if (sound_notify2(self, notif, flags, id) == -1)
        return -1;
    
    control_lock();
    
    if (!actives[id].box || actives[id].size >= 128 ) {
        control_unlock();
        return -1;
    }
    
    
    va_list __ARGS__; va_start (__ARGS__, format);
    vsnprintf (actives[id].messages[actives[id].size], 127, format, __ARGS__);
    va_end (__ARGS__);
    
    if (strlen(actives[id].messages[actives[id].size]) > 124)
        strcpy(actives[id].messages[actives[id].size] + 124, "...");
    
    actives[id].size ++;
    actives[id].n_timeout = time(NULL) + Control.notif_timeout / 1000;
    
    char formated[128 * 129] = {'\0'};
    
    int i = 0;
    for (; i <actives[id].size; i ++) {
        strcat(formated, actives[id].messages[i]);
        strcat(formated, "\n");
    }
    
    formated[strlen(formated) - 1] = '\0';
    
    notify_notification_update(actives[id].box, actives[id].title, formated, NULL);
    notify_notification_show(actives[id].box, NULL);
    
    control_unlock();
    
    return id;
#else
    return sound_notify2(self, notif, flags, id);
#endif
}

int box_silent_notify(ToxWindow* self, uint64_t flags, int* id_indicator, const char* title, const char* format, ...)
{
    /* Always do colored notify */
    if (self && self->alert == WINDOW_ALERT_NONE) {
        if (flags & NT_WNDALERT_0) self->alert = WINDOW_ALERT_0;
        else if (flags & NT_WNDALERT_1) self->alert = WINDOW_ALERT_1;
        else if (flags & NT_WNDALERT_2) self->alert = WINDOW_ALERT_2;
    }
    
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
        
#ifdef _BOX_NOTIFY
    
    control_lock();
    
    int id;        
    for (id = 0; id < ACTIVE_NOTIFS_MAX && actives[id].active; id ++);
    if ( id == ACTIVE_NOTIFS_MAX ) {
        control_unlock();
        return -1; /* Full */
    }
    
    if (id_indicator) {
        actives[id].id_indicator = id_indicator;
        *id_indicator = id;
    }
    
    strncpy(actives[id].title, title, 24);
    if (strlen(title) > 23) strcpy(actives[id].title + 20, "...");
    
    va_list __ARGS__; va_start (__ARGS__, format);
    vsnprintf (actives[id].messages[0], 127, format, __ARGS__);
    va_end (__ARGS__);
    
    if (strlen(actives[id].messages[0]) > 124) 
        strcpy(actives[id].messages[0] + 124, "...");
    
    actives[id].active = 1;
    actives[id].box = notify_notification_new(actives[id].title, actives[id].messages[0], NULL);
    actives[id].size ++;
    actives[id].n_timeout = time(NULL) + Control.notif_timeout / 1000;
    
    notify_notification_set_timeout(actives[id].box, Control.notif_timeout);
    notify_notification_set_app_name(actives[id].box, "toxic");
    /*notify_notification_add_action(actives[id].box, "lel", "default", m_notify_action, self, NULL);*/
    notify_notification_show(actives[id].box, NULL);
    
    control_unlock();
    return id;
#else
    return -1;
#endif
}

int box_silent_notify2(ToxWindow* self, uint64_t flags, int id, const char* format, ...)
{
    /* Always do colored notify */
    if (self && self->alert == WINDOW_ALERT_NONE) {
        if (flags & NT_WNDALERT_0) self->alert = WINDOW_ALERT_0;
        else if (flags & NT_WNDALERT_1) self->alert = WINDOW_ALERT_1;
        else if (flags & NT_WNDALERT_2) self->alert = WINDOW_ALERT_2;
    }
    
    if ((flags & NT_RESTOL && Control.cooldown > time(NULL)) ||
       ((flags & NT_NOFOCUS && Control.this_window == get_focused_window_id()) ))
        return -1;
        
#ifdef _BOX_NOTIFY
    control_lock();
    
    if (id < 0 || id >= ACTIVE_NOTIFS_MAX || !actives[id].box || actives[id].size >= 128 ) {
        control_unlock();
        return -1;
    }
        
    
    va_list __ARGS__; va_start (__ARGS__, format);
    vsnprintf (actives[id].messages[actives[id].size], 127, format, __ARGS__);
    va_end (__ARGS__);
    
    if (strlen(actives[id].messages[actives[id].size]) > 124)
        strcpy(actives[id].messages[actives[id].size] + 124, "...");
    
    actives[id].size ++;
    actives[id].n_timeout = time(NULL) + Control.notif_timeout / 1000;
    
    char formated[128 * 129] = {'\0'};
    
    int i = 0;
    for (; i <actives[id].size; i ++) {
        strcat(formated, actives[id].messages[i]);
        strcat(formated, "\n");
    }
    
    formated[strlen(formated) - 1] = '\0';
    
    notify_notification_update(actives[id].box, actives[id].title, formated, NULL);
    notify_notification_show(actives[id].box, NULL);
    
    control_unlock();
    
    return id;
#else
    return -1;
#endif
}
