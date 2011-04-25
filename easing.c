/*
 Copyright (C) 2010, 2011 Philipp Merkel <linux@philmerk.de>

 Permission to use, copy, modify, and/or distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.

 THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <X11/Xlib.h>
#include "twofingemu.h"
#include "easing.h"
#include "gestures.h"
#include <unistd.h>


/* Variables for easing: */
/* The thread used for easing */
pthread_t easingThread;
/* Set to stop easing thread (as calling pthread_cancel didn't work well) */
int stopEasing = 0;
/* 1 if an easing thread is currently running. */
int easingThreadActive = 0;
/* 1 if there is currently easing going on */
int easingActive = 0;
/* Mutex for starting/stopping the easing thread */
pthread_mutex_t easingMutex = PTHREAD_MUTEX_INITIALIZER;  
/* Condition variable for the easing thread to wait */
int easingWakeup = 0;
pthread_cond_t  easingWaitingCond = PTHREAD_COND_INITIALIZER;
/* Start Interval between two easing steps */
int easingInterval = 0;
/* 1 if easing shall move to the right, -1 if it shall move to the left. */
int easingDirectionX = 0;
/* 1 if easing shall move down, -1 if it shall move up. */
int easingDirectionY = 0;
/* The profile for the easing */
Profile* easingProfile;


/* Thread responsible for easing */
void* easingThreadFunction(void* arg) {
	return NULL;
}

/* Starts the easing; profile, interval and directions have to be set before. */
void startEasing(Profile * profile, int directionX, int directionY, int interval) {
}
/* Stops the easing. */
void stopEasingThread() {
}


