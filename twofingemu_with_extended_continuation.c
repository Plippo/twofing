/*
 Copyright (C) 2010, Philipp Merkel <linux@philmerk.de>

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

#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include "twofingemu.h"
#include "profiles.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int debugMode = 0;
/* Clickmode: 0 - first finger; 1 - second finger; 2 - center */
int clickmode = 2;

/* Maximum amount of milliseconds between two scrolling steps before easing stops. */
#define MAX_EASING_INTERVAL 200
/* Maximum amount of milliseconds between the last two scrolling steps for easing to start. */
#define MAX_EASING_START_INTERVAL 200

/* Number of milliseconds before a single click is registered, to give the user time to put down
   second finger for two-finger gestures. */
#define CLICK_DELAY 200

/* Continuation mode -- when 1, two finger gesture is continued when one finger is released.
   When 2, two finger gesture is continued even if both are released for a short time. */
#define CONTINUATION 2

#if CONTINUATION
	#define TWO_FINGERS_DOWN fingersDown == 2 && hadTwoFingersOn == 0
	#define TWO_FINGERS_ON fingersDown > 0 && hadTwoFingersOn == 1
	#define TWO_FINGERS_UP fingersDown == 0 && hadTwoFingersOn == 1
#else
	#define TWO_FINGERS_DOWN fingersDown == 2 && fingersWereDown < 2
	#define TWO_FINGERS_ON fingersDown == 2
	#define TWO_FINGERS_UP fingersDown < 2 && fingersWereDown == 2
#endif

/* Daemonize. Source: http://www-theorie.physik.unizh.ch/~dpotter/howto/daemonize (public domain) */
static void daemonize(void) {
	pid_t pid, sid;

	/* already a daemon */
	if (getppid() == 1)
		return;

	/* Fork off the parent process */
	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}
	/* If we got a good PID, then we can exit the parent process. */
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	/* At this point we are executing as the child process */

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		exit(EXIT_FAILURE);
	}

	/* Change the current working directory.  This prevents the current
	 directory from being locked; hence not being able to remove it. */
	if ((chdir("/")) < 0) {
		exit(EXIT_FAILURE);
	}

	/* Redirect standard files to /dev/null */
	void* r;
	r = freopen("/dev/null", "r", stdin);
	r = freopen("/dev/null", "w", stdout);
	r = freopen("/dev/null", "w", stderr);
}

/* Finger information */
FingerInfo fingerInfos[2] = { { .x=0, .y=0, .down = 0, .id = 0 }, { .x=0, .y=0,
		.down = 0, .id = 1 } };

/* X stuff */
Display* display;
Window root;
int screenNum;
int deviceID;
Atom WM_CLASS;
pthread_t xLoopThread;
Time fairlyCurrentTime;

/* Calibration data */
int calibMinX, calibMaxX, calibMinY, calibMaxY;
double calibFactorX, calibFactorY;
unsigned char calibSwapX, calibSwapY, calibSwapAxes;

/* The width and height of the screen in pixels */
unsigned int screenWidth, screenHeight;

/* Finger data */
int fingersDown = 0;
int fingersWereDown = 0;
/* Has button press of first button been called in XTest? */
int buttonDown = 0;
/* Maximum distance that two fingers have been moved while they were on */
double maxDist;
/* The time when the first finger touched */
Time fingerDownTime;
/* Were there once two fingers on during the current touch phase? */
int hadTwoFingersOn = 0;
/* Two finger gestures activated (and input grabbed)? */
int active = 0;
/* Activate two finger gestures (and grab input) as soon as the fingers have been removed */
int activateAtRelease = 0;

/* Gesture information */
int amPerformingGesture = 0;
/* Current profile has scrollBraceAction, thus mouse pointer has to be moved during scrolling */
int dragScrolling = 0;

/* position of center between fingers at start of gesture */
int gestureStartCenterX, gestureStartCenterY;
/* distance between two fingers at start of gesture */
double gestureStartDist;
/* angle of two fingers at start of gesture */
double gestureStartAngle;
/* current position of center between fingers */
int currentCenterX, currentCenterY;

#define PI 3.141592654

/* Profile of current window, if activated */
Profile* currentProfile = NULL;

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
/* 1 if the last X scroll command was to the right, -1 if it was to the left. */
int easingDirectionX = 0;
/* 1 if the last Y scroll command was down, -1 if it was up. */
int easingDirectionY = 0;
/* The profile for the easing */
Profile* easingProfile;
/* Time last scroll command in X/Y action was sent. */
Time lastScrollXTime;
Time lastScrollYTime;
/* Interval between the last two X/Y scroll commands. */
int lastScrollXIntv;
int lastScrollYIntv;
/*  Last values of lastScrollXIntv/lastScrollYIntv. */
int lastLastScrollXIntv;
int lastLastScrollYIntv;

/* Variables for extended continuation */
/* The thread used for extended continuation */
pthread_t continuationThread;
/* Mutex for starting/stopping the continuation thread */
pthread_mutex_t continuationMutex = PTHREAD_MUTEX_INITIALIZER;  
/* Condition variable for the easing thread to wait */
int continuationWakeup = 0;
pthread_cond_t continuationWaitingCond = PTHREAD_COND_INITIALIZER;
/* Called when extended continuation shall not be started again (e.g. because it has just been stopped) */
int dontStartContinuation = 0;

#define CONTINUATION_TIME 500

/* We are currently in "ignore release" phase to check if release of fingers has only been for short time */
int ignoreFingersUp = 0;


/* Handle errors by, well, throwing them away. */
int invalidWindowHandler(Display *dsp, XErrorEvent *err) {
	return 0;
}

/* Grab the device so input is captured */
void grab(Display* display, int grabDeviceID) {
	XIEventMask device_mask;
	unsigned char mask_data[1] = { 0 };
	device_mask.mask_len = sizeof(mask_data);
	device_mask.mask = mask_data;
	XISetMask(device_mask.mask, XI_ButtonPress);
	XISetMask(device_mask.mask, XI_ButtonRelease);
	XISetMask(device_mask.mask, XI_Motion);

	XIGrabModifiers modifiers[1] = { { 0, 0 } };

	XIGrabButton(display, grabDeviceID, 1, root, None, GrabModeAsync,
			GrabModeAsync, False, &device_mask, 1, modifiers);

}

/* Ungrab the device so input can be handled by application directly */
void ungrab(Display* display, int grabDeviceID) {
	XIGrabModifiers modifiers[1] = { { 0, 0 } };
	XIUngrabButton(display, grabDeviceID, 1, root, 1, modifiers);

}

/* Activates two finger gesture handling and input grabbing */
void activate() {
	activateAtRelease = 0;
	if (active == 0) {
		grab(display, deviceID);
		active = 1;
	}
}

/* Deactivates two finger gesture handling and input grabbing */
void deactivate() {
	activateAtRelease = 0;
	if (active) {
		active = 0;
		ungrab(display, deviceID);
	}
}

/* Send an XTest event to release the first button if it is currently pressed */
void releaseButton() {
	if (buttonDown) {
		buttonDown = 0;
		XTestFakeButtonEvent(display, 1, False, CurrentTime);
		XFlush(display);
	}
}

/* Executes the given action -- synthesizes key/button press, release or both, depending
 * on value of whatToDo (EXECUTEACTION_PRESS/_RELEASE/_BOTH). */
void executeAction(Action* action, int whatToDo) {
	if (whatToDo & EXECUTEACTION_PRESS) {
		if (action->actionType != ACTIONTYPE_NONE && action->modifier != 0) {
			if (action->modifier & MODIFIER_SHIFT) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Shift_L), True,
						CurrentTime);
			}
			if (action->modifier & MODIFIER_CONTROL) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display,
						XK_Control_L), True, CurrentTime);
			}
			if (action->modifier & MODIFIER_ALT) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_Alt_L),
						True, CurrentTime);
			}
			if (action->modifier & MODIFIER_SUPER) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Super_L), True,
						CurrentTime);
			}
		}

		switch (action->actionType) {
		case ACTIONTYPE_BUTTONPRESS:
			XTestFakeButtonEvent(display, action->keyButton, True, CurrentTime);
			XFlush(display);
			break;
		case ACTIONTYPE_KEYPRESS:
			XTestFakeKeyEvent(display, XKeysymToKeycode(display,
					action->keyButton), True, CurrentTime);
			XFlush(display);
			break;
		}

	}

	if (whatToDo & EXECUTEACTION_RELEASE) {

		switch (action->actionType) {
		case ACTIONTYPE_BUTTONPRESS:
			XTestFakeButtonEvent(display, action->keyButton, False, CurrentTime);
			XFlush(display);
			break;
		case ACTIONTYPE_KEYPRESS:
			XTestFakeKeyEvent(display, XKeysymToKeycode(display,
					action->keyButton), False, CurrentTime);
			XFlush(display);
			break;
		}

		if (action->actionType != ACTIONTYPE_NONE && action->modifier != 0) {
			if (action->modifier & MODIFIER_SHIFT) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Shift_L), False,
						CurrentTime);
			}
			if (action->modifier & MODIFIER_CONTROL) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display,
						XK_Control_L), False, CurrentTime);
			}
			if (action->modifier & MODIFIER_ALT) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_Alt_L),
						False, CurrentTime);
			}
			if (action->modifier & MODIFIER_SUPER) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Super_L), False,
						CurrentTime);
			}
		}
	}

}

/* Returns the active top-level window. A top-level window is one that has WM_CLASS set.
 * May also return None. */
Window getCurrentWindow() {
	//if(debugMode) printf("Called getCurrentWindow\n");
	/* First get the window that has the input focus */
	Window currentWindow;
	int revert;
	XGetInputFocus(display, &currentWindow, &revert);

	if (currentWindow == None) {
		//if(debugMode) printf("Leave getCurrentWindow\n");
		return currentWindow;
	}

	/* Now go through parent windows until we find one with WM_CLASS set. */
	Window root, parent;
	Window* childWindows = NULL;
	unsigned int childCount;


	XClassHint* classHint = XAllocClassHint();
	if(classHint == NULL) {
		if(debugMode) printf("Couldn't allocate class hint!!\n");
		return None;
	}

	int i = 0;
	while (1) {
		//if(debugMode) printf("in Loop\n");
		i++;
		if(i >= 5) {
			if(debugMode) printf("Too many iterations in getCurrentWindow\n");
			XFree(classHint);
			//if(debugMode) printf("Leave getCurrentWindow\n");
			return None;
		}
		if (currentWindow == root || currentWindow == None) {
			//if(debugMode) printf("Reached root!\n");
			/* No top-level window available. Should never happen. */
			XFree(classHint);
			//if(debugMode) printf("Leave getCurrentWindow\n");
			return currentWindow;
		}

		//if(debugMode) printf("Call XGetClassHint!\n");
		if (XGetClassHint(display, currentWindow, classHint) == 0) {
			//if(debugMode) printf("Has no Class!\n");
			/* Has no WM_CLASS, thus no top-level window */
			if(XQueryTree(display, currentWindow, &root, &parent, &childWindows,
					&childCount)) {
				//if(debugMode) printf("Queried tree!\n");

				if (childWindows != NULL)
					XFree(childWindows);

				if(currentWindow == parent) {
					if(debugMode) printf("parent same as current!\n");
					/* something wrong */
					XFree(classHint);
					//if(debugMode) printf("Leave getCurrentWindow\n");
					return currentWindow;
				}
				/* Continue with parent until we find WM_CLASS */
				currentWindow = parent;
			} else {
				//if(debugMode) printf("Could not query tree!\n");
				XFree(classHint);
				//if(debugMode) printf("Leave getCurrentWindow\n");
				return currentWindow;
			}
		} else {
			//if(debugMode) printf("Clean up class name!\n");
			if(classHint->res_class != NULL) XFree(classHint->res_class);
			if(classHint->res_name != NULL) XFree(classHint->res_name);
			XFree(classHint);
			//if(debugMode) printf("Leave getCurrentWindow\n");
			return currentWindow;
		}
	}
}

/* Returns a pointer to the profile of the currently selected
 * window, or defaultProfile if there is no specific profile for it or the window is invalid. */
Profile* getWindowProfile(Window w) {
	if (w != None) {

		XClassHint* classHint = XAllocClassHint();

		/* Read WM_CLASS member */
		if (XGetClassHint(display, w, classHint)) {

			if(debugMode) {
				printf("Current window: '%s'\n", classHint->res_name);
			}

			int i;
			/* Look for the profile with this class */
			for (i = 0; i < profileCount; i++) {
				if (strncmp(classHint->res_name, profiles[i].windowClass, 30)
						== 0) {
					if(classHint->res_class != NULL) XFree(classHint->res_class);
					if(classHint->res_name != NULL) XFree(classHint->res_name);
					XFree(classHint);
					/* Return this profile */
					return &profiles[i];
				}
			}

			if(classHint->res_class != NULL) XFree(classHint->res_class);
			if(classHint->res_name != NULL) XFree(classHint->res_name);
			XFree(classHint);
			/* No profile found, return default. */
			return &defaultProfile;
		} else {
			/* Couldn't get WM_CLASS, return default profile. */
			XFree(classHint);
			return &defaultProfile;
		}
	} else {
		/* Invalid window, return default window. */
		return &defaultProfile;
	}

}

/* Process the finger data gathered from the last set of events */
void processFingers() {
	//TODO lock
	int fingersOnlyPretended = 0;
	if(ignoreFingersUp) {
		if(fingersDown == 0) {
			fingersDown = 1;
			fingersOnlyPretended = 1;
		} else {
			ignoreFingersUp = 0;
		}
	}

	/* If we should activate at release, do it. */
	if (buttonDown == 0 && fingersDown == 0 && activateAtRelease != 0) {
		releaseButton();
		activate();
	}

	if (active == 0)
		return;

	if((fingersDown != 0 && fingersWereDown == 0) || (fingersDown == 2 && fingersWereDown == 1 && CONTINUATION == 2)) {
		stopEasingThread();
	}

	if (TWO_FINGERS_DOWN) {
		/* Second finger touched (and maybe first too) */

		lastScrollXTime = fairlyCurrentTime;
		lastScrollYTime = fairlyCurrentTime;
		lastScrollXIntv = 0; lastScrollYIntv = 0;		
		lastLastScrollXIntv = 0; lastLastScrollYIntv = 0;		

		maxDist = 0;

		/* Memorize that there were two fingers on during touch */
		hadTwoFingersOn = 1;

		/* Get current profile */
		currentProfile = getWindowProfile(getCurrentWindow());
		if(debugMode) {
			if(currentProfile->windowClass != NULL) {
				printf("Use profile '%s'\n", currentProfile->windowClass);
			} else {
				printf("Use default profile.\n");
			}
		}


		/* If there had already been a single-touch event raised because the
		 * user was too slow, stop it now. */
		releaseButton();

		/* Calculate center position and distance between touch points */
		gestureStartCenterX = (fingerInfos[0].x + fingerInfos[1].x) / 2;
		gestureStartCenterY = (fingerInfos[0].y + fingerInfos[1].y) / 2;

		int xdiff = fingerInfos[1].x - fingerInfos[0].x;
		int ydiff = fingerInfos[1].y - fingerInfos[0].y;
		gestureStartDist = sqrt(xdiff * xdiff + ydiff * ydiff);
		gestureStartAngle = atan2(ydiff, xdiff) * 180 / PI;

		/* We have not decided on a gesture yet. */
		amPerformingGesture = GESTURE_UNDECIDED;

		/* Move pointer to center between touch points */
		XTestFakeMotionEvent(display, -1, gestureStartCenterX, gestureStartCenterY, CurrentTime);
		XFlush(display);
	} else if (TWO_FINGERS_ON) {
		if(!fingersOnlyPretended) {

			/* Moved with two fingers */

			if(fingersDown == 2) {
				/* Calculate new center between fingers */
				currentCenterX = (fingerInfos[0].x + fingerInfos[1].x) / 2;
				currentCenterY = (fingerInfos[0].y + fingerInfos[1].y) / 2;
			} else {
				int i;
				for(i = 0; i <= 1; i++) {
					if(fingerInfos[i].down) {
						currentCenterX = fingerInfos[i].x;
						currentCenterY = fingerInfos[i].y;
					}
				}
			}

			/* If we are dragScrolling (we are scrolling and there is a brace action,
			 * we need to move the pointer */
			if (amPerformingGesture == GESTURE_SCROLL && dragScrolling) {
				/* Move pointer to center between touch points */
				XTestFakeMotionEvent(display, -1, currentCenterX, currentCenterY, CurrentTime);
				XFlush(display);
			}

			/* Perform gestures as long as there are some. */
			while (checkGesture())
				;
		}
	} else if (TWO_FINGERS_UP) {
		/* Second finger (and maybe also first) released */

		/* !easingActive is necessary for following reason: When CONTINUATION is 2, TWO_FINGERS_UP may be 
                   called a second time once the continuation timer expires, but we don't want to start easing again then. */
		if (amPerformingGesture == GESTURE_SCROLL && !(easingActive)) {
			/* If there was a scroll gesture and we have a brace action, perform release. */
			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollBraceAction),
						EXECUTEACTION_RELEASE);
			} else {
				executeAction(&(currentProfile->scrollBraceAction),
						EXECUTEACTION_RELEASE);
			}
			/* Start easing (has no effect if we only have brace action */
			easingProfile = currentProfile;
			if(debugMode) printf("Start easing\n");

			/* Compensate for scrolling gestures getting a little bit slower at the end */
			if(lastLastScrollXIntv < lastScrollXIntv && lastLastScrollXIntv != 0) lastScrollXIntv = lastLastScrollXIntv;
			if(lastLastScrollYIntv < lastScrollYIntv && lastLastScrollYIntv != 0) lastScrollYIntv = lastLastScrollYIntv;

			/* Check if scrolling intervals are not too long. Also check if last scrolling on an axis is longer
			   ago than twice its interval, which means the scrolling has been stopped or extremely slowed down since. */
			if(lastScrollYIntv == 0 || fairlyCurrentTime - lastScrollYTime > lastScrollYIntv * 2 || lastScrollYIntv > MAX_EASING_START_INTERVAL) easingDirectionY = 0;
			if(lastScrollXIntv == 0 || fairlyCurrentTime - lastScrollXTime > lastScrollXIntv * 2 || lastScrollXIntv > MAX_EASING_START_INTERVAL) easingDirectionX = 0;
			if(easingDirectionX != 0 || easingDirectionY != 0) {
				if(easingDirectionX != 0 && easingDirectionY != 0) {
					/* As we only support one interval, only use larger axis. */
					if(lastScrollXIntv < lastScrollYIntv) {
						easingDirectionY = 0;
					} else if(lastScrollYIntv < lastScrollXIntv) {
						easingDirectionX = 0;
					}
				}
				if(easingDirectionY == 0) {
					easingInterval = lastScrollXIntv;
				} else if(easingDirectionX == 0) {
					easingInterval = lastScrollYIntv;
				}
				startEasingThread();
			}
		}

		if(amPerformingGesture != GESTURE_NONE && amPerformingGesture != GESTURE_UNDECIDED && CONTINUATION == 2 && !dontStartContinuation) {
			startContinuation();
			// continuation thread shall call method with fingersDown == 0 once timer expires.
			fingersDown = 1;
		} else {
			/* If we haven't performed a gesture and haven't moved too far, perform tap action. */
			if ((amPerformingGesture == GESTURE_NONE || amPerformingGesture
					== GESTURE_UNDECIDED) && maxDist < 10) {
				/* Move pointer to correct position */
				if(clickmode == 2) {
					XTestFakeMotionEvent(display, -1, gestureStartCenterX, gestureStartCenterY, CurrentTime);
				} else {
					/* Assume first finger is at ID 0 and second finger at ID 1, might have to be changed later */
					XTestFakeMotionEvent(display, -1, fingerInfos[clickmode].x, fingerInfos[clickmode].y, CurrentTime);
				}
				XFlush(display);

				if (currentProfile->tapInherit) {
					executeAction(&(defaultProfile.tapAction), EXECUTEACTION_BOTH);
				} else {
					executeAction(&(currentProfile->tapAction), EXECUTEACTION_BOTH);
				}
			}

			amPerformingGesture = GESTURE_NONE;
		}
	} else if (fingersDown == 1 && fingersWereDown == 0) {
		/* First finger touched */
		fingerDownTime = fairlyCurrentTime;
		/* Fake single-touch move event */
		int i;
		for(i = 0; i <= 1; i++) {
			if(fingerInfos[i].down) {
				XTestFakeMotionEvent(display, -1, fingerInfos[i].x, fingerInfos[i].y, CurrentTime);
				XFlush(display);
			}
		}

	} else if (fingersDown == 1) {
		/* Moved with one finger */
		if(!fingersOnlyPretended) {
			if (hadTwoFingersOn == 0 && buttonDown == 0) {
				if (fairlyCurrentTime > fingerDownTime + CLICK_DELAY) {
					/* Delay has passed, no gesture been performed, so perform single-touch press now */

					buttonDown = 1;
					XTestFakeButtonEvent(display, 1, True, CurrentTime);
					XFlush(display);
				}
			}
			if(buttonDown) {
				/* Fake single-touch move event */
				int i;
				for(i = 0; i <= 1; i++) {
					if(fingerInfos[i].down) {
						XTestFakeMotionEvent(display, -1, fingerInfos[i].x, fingerInfos[i].y, CurrentTime);
						XFlush(display);
					}
				}

			}
		}
	} else if (fingersDown == 0 && fingersWereDown > 0) {
		/* Last finger released */
		if (hadTwoFingersOn == 0 && buttonDown == 0) {
			/* The button press time has not been reached yet, and we never had two
			 * fingers on (we could not have done this in this short time) so
			 * we simulate button down and up now. */
			XTestFakeButtonEvent(display, 1, True, CurrentTime);
			XFlush(display);
			XTestFakeButtonEvent(display, 1, False, CurrentTime);
			XFlush(display);
		} else {
			/* We release the button if it is down. */
			releaseButton();
		}
	}
	if (fingersDown == 0) {
		/* Reset fields after release */
		hadTwoFingersOn = 0;
	}
	/* Save number of fingers to compare next time */
	fingersWereDown = fingersDown;

	/* If we should activate at release, do it (now again because fingersDown might have changed). */
	if (buttonDown == 0 && fingersDown == 0 && activateAtRelease != 0) {
		releaseButton();
		activate();
	}
}

/* All the gesture-related code.
 * Returns 1 if the method should be called again, 0 otherwise.
 */
int checkGesture() {

	/* Calculate difference between two touch points and angle */
	int xdiff = fingerInfos[1].x - fingerInfos[0].x;
	int ydiff = fingerInfos[1].y - fingerInfos[0].y;
	double currentDist = sqrt(xdiff * xdiff + ydiff * ydiff);
	double currentAngle = atan2(ydiff, xdiff) * 180 / PI;

	/* Check distance the fingers (more exactly: the point between them)
	 * has been moved since start of the gesture. */
	int xdist = currentCenterX - gestureStartCenterX;
	int ydist = currentCenterY - gestureStartCenterY;
	double moveDist = sqrt(xdist * xdist + ydist * ydist);
	if (moveDist > maxDist && fingersDown == 2)
		maxDist = moveDist;

	/* We don't know yet what to do, so look if we can decide now. */
	if (amPerformingGesture == GESTURE_UNDECIDED && fingersDown == 2) {
		int scrollMinDist = currentProfile->scrollMinDistance;
		if (currentProfile->scrollInherit)
			scrollMinDist = defaultProfile.scrollMinDistance;
		if ((int) moveDist > scrollMinDist) {
			amPerformingGesture = GESTURE_SCROLL;
			if(debugMode) printf("Start scrolling gesture\n");

			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollBraceAction),
						EXECUTEACTION_PRESS);
				if (defaultProfile.scrollBraceAction.actionType
						== ACTIONTYPE_NONE) {
					dragScrolling = 0;
				} else {
					dragScrolling = 1;
				}
			} else {
				executeAction(&(currentProfile->scrollBraceAction),
						EXECUTEACTION_PRESS);
				if (currentProfile->scrollBraceAction.actionType
						== ACTIONTYPE_NONE) {
					dragScrolling = 0;
				} else {
					dragScrolling = 1;
				}
			}
			return 1;
		}

		int zoomMinDist = currentProfile->zoomMinDistance;
		if (currentProfile->zoomInherit)
			zoomMinDist = defaultProfile.zoomMinDistance;
		if (abs((int) currentDist - gestureStartDist) > zoomMinDist) {
			amPerformingGesture = GESTURE_ZOOM;
			if(debugMode) printf("Start zoom gesture\n");
			return 1;
		}

		int rotateMinDist = currentProfile->rotateMinDistance;
		double rotateMinAngle = currentProfile->rotateMinAngle;
		if (currentProfile->rotateInherit) {
			rotateMinDist = defaultProfile.rotateMinDistance;
			rotateMinAngle = defaultProfile.rotateMinAngle;
		}
		double rotatedBy = currentAngle - gestureStartAngle;
		if (rotatedBy < -180)
			rotatedBy += 360;
		if (rotatedBy > 180)
			rotatedBy -= 360;
		//printf("Rotated by: %f; min. angle: %f\n", rotatedBy, rotateMinAngle);
		if (abs(rotatedBy) > rotateMinAngle && (int) currentDist
				> rotateMinDist) {
			amPerformingGesture = GESTURE_ROTATE;
			if(debugMode) printf("Start rotation gesture\n");
			return 1;
		}
	}

	/* If we know hat gesture to perform, look if there is something to do */
	switch (amPerformingGesture) {
	case GESTURE_SCROLL:
		;
		int hscrolledBy = currentCenterX - gestureStartCenterX;
		int vscrolledBy = currentCenterY - gestureStartCenterY;
		int hscrollStep = currentProfile->hscrollStep;
		int vscrollStep = currentProfile->vscrollStep;
		if (currentProfile->scrollInherit) {
			hscrollStep = defaultProfile.hscrollStep;
			vscrollStep = defaultProfile.vscrollStep;
		}
		if (hscrollStep == 0 || vscrollStep == 0)
			return 0;

		if (hscrolledBy > hscrollStep) {
			easingDirectionX = 1;
			lastLastScrollXIntv = lastScrollXIntv;
			lastScrollXIntv = fairlyCurrentTime - lastScrollXTime;
			lastScrollXTime = fairlyCurrentTime;
			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollRightAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->scrollRightAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartCenterX = gestureStartCenterX + hscrollStep;
			return 1;
		} else if (hscrolledBy < -hscrollStep) {
			lastLastScrollXIntv = lastScrollXIntv;
			lastScrollXIntv = fairlyCurrentTime - lastScrollXTime;
			lastScrollXTime = fairlyCurrentTime;
			easingDirectionX = -1;
			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollLeftAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->scrollLeftAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartCenterX = gestureStartCenterX - hscrollStep;
			return 1;
		}
		if (vscrolledBy > vscrollStep) {
			lastLastScrollYIntv = lastScrollYIntv;
			lastScrollYIntv = fairlyCurrentTime - lastScrollYTime;
			lastScrollYTime = fairlyCurrentTime;
			easingDirectionY = 1;
			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollDownAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->scrollDownAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartCenterY = gestureStartCenterY + vscrollStep;
			return 1;
		} else if (vscrolledBy < -vscrollStep) {
			lastLastScrollYIntv = lastScrollYIntv;
			lastScrollYIntv = fairlyCurrentTime - lastScrollYTime;
			lastScrollYTime = fairlyCurrentTime;
			easingDirectionY = -1;
			if (currentProfile->scrollInherit) {
				executeAction(&(defaultProfile.scrollUpAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->scrollUpAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartCenterY = gestureStartCenterY - vscrollStep;
			return 1;
		}

		return 0;
	case GESTURE_ZOOM:
		;
		double zoomedBy = currentDist / gestureStartDist;
		double zoomStep = currentProfile->zoomStep;
		if (currentProfile->zoomInherit)
			zoomStep = defaultProfile.zoomStep;
		if (zoomedBy > zoomStep) {
			if(debugMode) printf("Zoom in step\n");
			if (currentProfile->zoomInherit) {
				executeAction(&(defaultProfile.zoomInAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->zoomInAction),
						EXECUTEACTION_BOTH);
			}
			/* Reset distance */
			gestureStartDist = gestureStartDist * zoomStep;
			return 1;
		} else if (zoomedBy < 1 / zoomStep) {
			if(debugMode) printf("Zoom out step\n");
			if (currentProfile->zoomInherit) {
				executeAction(&(defaultProfile.zoomOutAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->zoomOutAction),
						EXECUTEACTION_BOTH);
			}
			/* Reset distance */
			gestureStartDist = gestureStartDist / zoomStep;
			return 1;
		}
		return 0;
	case GESTURE_ROTATE:
		;
		double rotatedBy = currentAngle - gestureStartAngle;
		if (rotatedBy < -180)
			rotatedBy += 360;
		if (rotatedBy > 180)
			rotatedBy -= 360;
		double rotateStep = currentProfile->rotateStep;
		if (currentProfile->rotateInherit)
			rotateStep = defaultProfile.rotateStep;
		if (rotatedBy > rotateStep) {
			if(debugMode) printf("Rotate right\n");
			if (currentProfile->rotateInherit) {
				executeAction(&(defaultProfile.rotateRightAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->rotateRightAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartAngle = gestureStartAngle + rotateStep;
		} else if (rotatedBy < -rotateStep) {
			if(debugMode) printf("Rotate left\n");
			if (currentProfile->rotateInherit) {
				executeAction(&(defaultProfile.rotateLeftAction),
						EXECUTEACTION_BOTH);
			} else {
				executeAction(&(currentProfile->rotateLeftAction),
						EXECUTEACTION_BOTH);
			}

			gestureStartAngle = gestureStartAngle - rotateStep;
		}

		return 0;
	}

	return 0;

}

/* Called when a blacklisted window is left (for some reasons sometimes also if it's not blacklisted) */
void leaveWindow() {
	if (active == 0) {

		activateAtRelease = 1;
	}
}

/* Called when a blacklisted window is focused */
void enterBlacklistedWindow() {
	//printf("Entered bad window.\n");
	activateAtRelease = 0;
	releaseButton();
	deactivate();
}

/* Returns whether the given window is blacklisted */
int isWindowBlacklisted(Window w) {
	if(w == None) return 0;
	int i;

	XClassHint* classHint = XAllocClassHint();

	if (XGetClassHint(display, w, classHint)) {
		if(debugMode) {
			printf("Found window with id %i and class '%s' \n", (int) w,
					classHint->res_name);
		}

		for (i = 0; i < blacklistLength; i++) {
			if (strncmp(classHint->res_name, blacklist[i], 30) == 0) {
				if(classHint->res_class != NULL) XFree(classHint->res_class);
				if(classHint->res_name != NULL) XFree(classHint->res_name);
				XFree(classHint);
				return 1;
			}
		}

		if(classHint->res_class != NULL) XFree(classHint->res_class);
		if(classHint->res_name != NULL) XFree(classHint->res_name);
	} else {
		printf("Found window with id %i and no class.\n", (int) w);
	}
	XFree(classHint);
	return 0;
}

/* Called when a new window is mapped. Checks whether it is blacklisted and registers
 * the activation events if yes. Then checks if it is active and, if yes, calls enter/leave.
 */
void windowMapped(Window w) {

	if (isWindowBlacklisted(w)) {
		if(debugMode) printf("It's blacklisted.\n");
		/* register for window focus events */
		XSelectInput(display, w, EnterWindowMask | LeaveWindowMask);
		if (getCurrentWindow() == w) {
			/* If this is current and blacklisted, call enterBlacklistedWindow */
			enterBlacklistedWindow();
		}
	} else {
		if(debugMode) printf("It's not blacklisted.\n");
		if (getCurrentWindow() == w) {
			if(debugMode) printf("It's the current one!\n");
			/* It is current and not blacklisted, so we might have left a blacklisted window */
			leaveWindow();
		}
	}

}

void setScreenSize(XRRScreenChangeNotifyEvent * evt) {
	screenWidth = evt->width;
	screenHeight = evt->height;
	if(calibMaxX != calibMinX && calibMaxY != calibMinY) {
		calibFactorX = ( screenWidth / (double) (calibMaxX - calibMinX));
		calibFactorY = ( screenHeight / (double) (calibMaxY - calibMinY));
	} else {
		calibFactorX = 1.;
		calibFactorY = 1.;
	}
	if(debugMode) {
		printf("New screen size: %i x %i\n", screenWidth, screenHeight);
	}
}


/* Thread responsible for easing */
void* easingThreadFunction(void* arg) {
	if(debugMode) printf("Easing Thread started\n");
	int nextInterval = easingInterval;
	int lastTime = 0;
	while(1) {
		usleep((nextInterval - lastTime) * 1000);

		pthread_mutex_lock(&easingMutex);
		if(stopEasing || nextInterval > MAX_EASING_INTERVAL) {
			stopEasing = 0;
			/* Wait until woken up */
			easingActive = 0;
			if(debugMode) printf("Easing thread goes to sleep. zZzZzZzZ...\n");
			while(!easingWakeup) {
				pthread_cond_wait(&easingWaitingCond, &easingMutex);
			}
			easingWakeup = 0;
			if(debugMode) printf("*rrrrring* Easing thread woken up!\n");
			easingActive = 1;
			nextInterval = easingInterval;
		}
		pthread_mutex_unlock(&easingMutex);

		if(debugMode) printf("Easing step\n");
		if (easingProfile->scrollInherit) {
			if(easingDirectionY == -1) {
				executeAction(&(defaultProfile.scrollUpAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionY == 1) {
				executeAction(&(defaultProfile.scrollDownAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionX == -1) {
				executeAction(&(defaultProfile.scrollLeftAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionX == 1) {
				executeAction(&(defaultProfile.scrollRightAction),
					EXECUTEACTION_BOTH);
			}
		} else {
			if(easingDirectionY == -1) {
				executeAction(&(easingProfile->scrollUpAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionY == 1) {
				executeAction(&(easingProfile->scrollDownAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionX == -1) {
				executeAction(&(easingProfile->scrollLeftAction),
					EXECUTEACTION_BOTH);
			}
			if(easingDirectionX == 1) {
				executeAction(&(easingProfile->scrollRightAction),
					EXECUTEACTION_BOTH);
			}
		}
		nextInterval = (int) (((float) nextInterval) * 1.15);
	}
	return NULL;
}

/* Starts the easing; profile, interval and directions have to be set before. */
void startEasingThread() {
	pthread_mutex_lock(&easingMutex);
	stopEasing = 0;
	if(!easingThreadActive) {
		pthread_create(&easingThread, NULL,
					easingThreadFunction, NULL);
		easingThreadActive = 1;
	} else {
		/* wake up easing thread */
		easingWakeup = 1;
		pthread_cond_broadcast(&easingWaitingCond);
	}
	pthread_mutex_unlock(&easingMutex);
}
/* Stops the easing. */
void stopEasingThread() {
	pthread_mutex_lock(&easingMutex);
	if(easingThreadActive) {
		stopEasing = 1;
	}
	pthread_mutex_unlock(&easingMutex);
}


void startContinuation() {
	pthread_mutex_lock(&continuationMutex);
	continuationWakeup = 1;
	pthread_cond_broadcast(&continuationWaitingCond);
	pthread_mutex_unlock(&continuationMutex);
}


/* Thread responsible for continuation */
void* continuationThreadFunction(void* arg) {
	while(1) {

		pthread_mutex_lock(&continuationMutex);
		/* Wait until woken up */
		if(debugMode) printf("Continuation thread goes to sleep. zZzZzZzZ...\n");
		while(!continuationWakeup) {
			pthread_cond_wait(&continuationWaitingCond, &continuationMutex);
		}
		continuationWakeup = 0;
		if(debugMode) printf("*rrrrring* Continuation thread woken up!\n");
		pthread_mutex_unlock(&continuationMutex);
		ignoreFingersUp = 1;
		usleep(CONTINUATION_TIME * 1000);
		//TODO lock gesture thread
		if(ignoreFingersUp) {
			ignoreFingersUp = 0;
			fingersDown = 0;
			dontStartContinuation = 1;
			processFingers();
			dontStartContinuation = 0;
		} else {
			if(debugMode) printf("Continuation success!\n");
		}
	}
}

/* X thread that processes X events in parallel to kernel device loop */
void* xLoopThreadFunction(void* arg) {
	XEvent ev;
	while (1) {
		XNextEvent(display, &ev);

		if (XGetEventData(display, &(ev.xcookie))) {
			XGenericEventCookie *cookie = &(ev.xcookie);
			if (cookie->evtype == XI_Motion) {
				/* Move event */
				XIDeviceEvent* data = cookie->data;
				fairlyCurrentTime = data->time;
//				if (!dontMove) {
//					/* Fake single-touch move event */
//					XTestFakeMotionEvent(display, -1, (int) data->root_x,
//							(int) data->root_y, CurrentTime);
//					XFlush(display);
//				}
			} else if (cookie->evtype == XI_PropertyEvent) {
				/* Device properties changed -> recalibrate. */
				printf("Device properties changed.\n");
				readCalibrationData(0);
			}

			XFreeEventData(display, &(ev.xcookie));

		} else {
			if (ev.type == MapNotify) {
				windowMapped(ev.xmap.window);
			} else if (ev.type == EnterNotify) {
				/* This is only called for blacklisted windows */
				enterBlacklistedWindow();
			} else if (ev.type == LeaveNotify) {
				/* This should only be called for blacklisted windows, but it's sometimes also
				 * called when a non-blacklisted window is left. But that's no problem for us. */
				leaveWindow();
			} else if(ev.type == 101) {
				/* Why isn't this magic constant explained anywhere?? */
				setScreenSize((XRRScreenChangeNotifyEvent *) &ev);
			}
		}
	}
}

/* Checks for all running windows if they are blacklisted and/or the current window.
 * Registers enter/leave event handlers for blacklisted windows. Executes them for
 * the current window. */
void checkRunningWindows() {
	Window aroot;
	Window parent;
	Window* childWindows = NULL;
	unsigned int childCount;
	/* Get all top-level windows */
	if(XQueryTree(display, root, &aroot, &parent, &childWindows, &childCount)) {

		if (childWindows != NULL) {
			int i;
			for (i = 0; i < childCount; i++) {
				windowMapped(childWindows[i]);
			}
			XFree(childWindows);
		}
	}
}

/* Reads the calibration data from evdev, should be self-explanatory. */
void readCalibrationData(int exitOnFail) {
	if(debugMode) {
		printf("Start calibration\n");
	}
	Atom retType;
	int retFormat;
	unsigned long retItems, retBytesAfter;
	unsigned int* data;
	if(XIGetProperty(display, deviceID, XInternAtom(display,
			"Evdev Axis Calibration", 0), 0, 4 * 32, False, XA_INTEGER,
			&retType, &retFormat, &retItems, &retBytesAfter,
			(unsigned char**) &data) != Success) {
		data = NULL;
	}

	if (data == NULL || retItems != 4 || data[0] == data[1] || data[2] == data[3]) {
		/* evdev might not be ready yet after resume. Let's wait a second and try again. */
		sleep(1);

		if(XIGetProperty(display, deviceID, XInternAtom(display,
				"Evdev Axis Calibration", 0), 0, 4 * 32, False, XA_INTEGER,
				&retType, &retFormat, &retItems, &retBytesAfter,
				(unsigned char**) &data) != Success) {
			return;
		}

		if (retItems != 4 || data[0] == data[1] || data[2] == data[3]) {

			if(debugMode) {
				printf("No calibration data found, use default values.\n");
			}

			/* Get minimum/maximum of axes */

			int nDev;
			XIDeviceInfo * deviceInfo = XIQueryDevice(display, deviceID, &nDev);

			int c;
			for(c = 0; c < deviceInfo->num_classes; c++) {
				if(deviceInfo->classes[c]->type == XIValuatorClass) {
					XIValuatorClassInfo* valuatorInfo = (XIValuatorClassInfo *) deviceInfo->classes[c];
					if(valuatorInfo->mode == XIModeAbsolute) {
						if(valuatorInfo->label == XInternAtom(display, "Abs X", 0)) {
							calibMinX = valuatorInfo->min;
							calibMaxX = valuatorInfo->max;
						} else if(valuatorInfo->label == XInternAtom(display, "Abs Y", 0)) {
							calibMinY = valuatorInfo->min;
							calibMaxY = valuatorInfo->max;
						}
					}
				}
			}

			XIFreeDeviceInfo(deviceInfo);

		} else {
			calibMinX = data[0];
			calibMaxX = data[1];
			calibMinY = data[2];
			calibMaxY = data[3];
		}
	} else {
		calibMinX = data[0];
		calibMaxX = data[1];
		calibMinY = data[2];
		calibMaxY = data[3];
	}
	calibFactorX = ( screenWidth / (double) (calibMaxX - calibMinX));
	calibFactorY = ( screenHeight / (double) (calibMaxY - calibMinY));
	printf("Calibration factors: %f %f\n", calibFactorX, calibFactorY);

	if(data != NULL) {
		XFree(data);
	}

	unsigned char* data2;

	if(XIGetProperty(display, deviceID, XInternAtom(display,
			"Evdev Axis Inversion", 0), 0, 2 * 8, False, XA_INTEGER, &retType,
			&retFormat, &retItems, &retBytesAfter, (unsigned char**) &data2) != Success) {
		return;
	}

	if (retItems != 2) {
		if (exitOnFail) {
			printf("No valid axis inversion data found.\n");
			exit(1);
		} else {
			return;
		}
	}

	calibSwapX = data2[0];
	calibSwapY = data2[1];

	XFree(data2);

	if(XIGetProperty(display, deviceID,
			XInternAtom(display, "Evdev Axes Swap", 0), 0, 8, False,
			XA_INTEGER, &retType, &retFormat, &retItems, &retBytesAfter,
			(unsigned char**) &data2) != Success) {
		return;
	}

	if (retItems != 1) {
		if (exitOnFail) {
			printf("No valid axes swap data found.\n");
			exit(1);
		} else {
			return;
		}
	}
	calibSwapAxes = data2[0];

	XFree(data2);

}

/* Main function, contains kernel driver event loop */
int main(int argc, char **argv) {

	char* devname = 0;
	int doDaemonize = 1;
	int doWait = 0;

	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			doDaemonize = 0;
			debugMode = 1;
		} else if (strcmp(argv[i], "--wait") == 0) {
			doWait = 1;
		} else if (strcmp(argv[i], "--click=first") == 0) {
			clickmode = 0;
		} else if (strcmp(argv[i], "--click=second") == 0) {
			clickmode = 1;
		} else if (strcmp(argv[i], "--click=center") == 0) {
			clickmode = 2;
		} else {
			devname = argv[i];
		}

	}



	if (doDaemonize) {
		daemonize();
	}

	if (doWait) {
		/* Wait until all necessary things are loaded */
		sleep(10);
	}

	/* We have two threads accessing X */
	XInitThreads();

	/* Connect to X server */
	if ((display = XOpenDisplay(NULL)) == NULL) {
		fprintf(stderr, "Couldn't connect to X server\n");
		exit(1);
	}

	/* Read X data */
	screenNum = DefaultScreen(display);

	root = RootWindow(display, screenNum);

//	realDisplayWidth = DisplayWidth(display, screenNum);
//	realDisplayHeight = DisplayHeight(display, screenNum);

	WM_CLASS = XInternAtom(display, "WM_CLASS", 0);

	/* Get notified about new windows */
	XSelectInput(display, root, StructureNotifyMask | SubstructureNotifyMask);

	//TODO load blacklist and profiles from file(s)

	/* Device file name */
	if (devname == 0) {
		devname = "/dev/twofingtouch";
	}

	/* Save return value of pthread_create so we only call it later if it hasn't been successfully called before. */
	int threadReturn = 1;

	/* Try to read from device file */
	int fileDesc;
	if ((fileDesc = open(devname, O_RDONLY)) < 0) {
		perror("twofing");
		return 1;
	}

	if(CONTINUATION == 2) {
		pthread_create(&continuationThread, NULL,
					continuationThreadFunction, NULL);
	}

	while (1) {
		/* Perform initialization at beginning and after module has been re-loaded */
		int rd, i;
		struct input_event ev[64];

		char name[256] = "Unknown";

		/* Read device name */
		ioctl(fileDesc, EVIOCGNAME(sizeof(name)), name);
		printf("Input device name: \"%s\"\n", name);

		XSetErrorHandler(invalidWindowHandler);


		int opcode, event, error;
		if (!XQueryExtension(display, "RANDR", &opcode, &event,
				&error)) {
			printf("X RANDR extension not available.\n");
			XCloseDisplay(display);
			exit(1);
		}

		/* Which version of XRandR? We support 1.3 */
		int major = 1, minor = 3;
		if (!XRRQueryVersion(display, &major, &minor)) {
			printf("XRandR version not available.\n");
			XCloseDisplay(display);
			exit(1);
		} else if(!(major>1 || (major == 1 && minor >= 3))) {
			printf("XRandR 1.3 not available. Server supports %d.%d\n", major, minor);
			XCloseDisplay(display);
			exit(1);
		}

		/* XInput Extension available? */
		if (!XQueryExtension(display, "XInputExtension", &opcode, &event,
				&error)) {
			printf("X Input extension not available.\n");
			XCloseDisplay(display);
			exit(1);
		}

		/* Which version of XI2? We support 2.0 */
		major = 2; minor = 0;
		if (XIQueryVersion(display, &major, &minor) == BadRequest) {
			printf("XI2 not available. Server supports %d.%d\n", major, minor);
			XCloseDisplay(display);
			exit(1);
		}

		screenWidth = XDisplayWidth(display, screenNum);
		screenHeight = XDisplayHeight(display, screenNum);

		int n;
		XIDeviceInfo *info = XIQueryDevice(display, XIAllDevices, &n);
		if (!info) {
			printf("No XInput devices available\n");
			exit(1);
		}

		/* Go through input devices and look for that with the same name as the given device */
		int devindex;
		for (devindex = 0; devindex < n; devindex++) {
			if (info[devindex].use == XIMasterPointer || info[devindex].use
					== XIMasterKeyboard)
				continue;

			if (strcmp(info[devindex].name, name) == 0) {
				deviceID = info[devindex].deviceid;

				break;
			}

		}
		if (deviceID == -1) {
			printf("Input device not found in XInput device list.\n");
			exit(1);
		}

		XIFreeDeviceInfo(info);

		if(debugMode) printf("XInput device id is %i.\n", deviceID);

		/* Prepare by reading calibration */
		readCalibrationData(1);

		/* Receive device property change events */
		XIEventMask device_mask2;
		unsigned char mask_data2[2] = { 0, 0 };
		device_mask2.deviceid = deviceID;
		device_mask2.mask_len = sizeof(mask_data2);
		device_mask2.mask = mask_data2;
		XISetMask(device_mask2.mask, XI_PropertyEvent);
		XISelectEvents(display, root, &device_mask2, 1);

		/* Recieve events when screen size changes */
		XRRSelectInput(display, root, RRScreenChangeNotifyMask);

		/* Needed for XTest to work correctly */
		XTestGrabControl(display, True);
		active = 0;
		checkRunningWindows();
		Window w = getCurrentWindow();
		activateAtRelease = 0;
		if(debugMode) printf("Current Window: %i\n", (int) w);

		if(isWindowBlacklisted(w)) {
			/* Current is blacklisted, call enterBlacklistedWindow */
			enterBlacklistedWindow();
		} else {
			/* Current is not blacklisted, so we might have left a blacklisted window */
			activate();
		}

		/* Needed for some reason to receive events */
		XGrabPointer(display, root, False, 0, GrabModeAsync, GrabModeAsync,
				None, None, CurrentTime);
		XUngrabPointer(display, CurrentTime);

		/* If it is not already running, create thread that listens to XInput events */
		if (threadReturn != 0)
			threadReturn = pthread_create(&xLoopThread, NULL,
					xLoopThreadFunction, NULL);

		printf("Reading input from device ... (interrupt to exit)\n");
		FingerInfo tempFingerInfo = { -1, -1, -1, -1 };

		/* Kernel device event loop */
		while (1) {
			rd = read(fileDesc, ev, sizeof(struct input_event) * 64);
			if (rd < (int) sizeof(struct input_event)) {
				printf("Data stream stopped\n");
				break;
			}
			for (i = 0; i < rd / sizeof(struct input_event); i++) {
				if (ev[i].type == EV_SYN) {

					if (2 == ev[i].code) { // MT_Sync : Multitouch event end
						/* Finger info for one finger collected in tempFingerInfo, so save it to fingerInfos. */
						/* Currently, we assume that the tracking ids of the fingers are 0,1 -- we should not do this but rather go through all fingers and check for the tracking id. We should also check if there are more than two fingers and set all to down then. */
						int index = tempFingerInfo.id;
						if (index == 0 || index == 1) {
							fingerInfos[index].id = tempFingerInfo.id;

							/* Calibrate coordinates */
							if (calibSwapAxes) {

								fingerInfos[index].x = (int)
										((tempFingerInfo.y - calibMinX) * calibFactorX);
								fingerInfos[index].y = (int)
										((tempFingerInfo.x - calibMinY) * calibFactorY);

							} else {

								fingerInfos[index].x = (int)
										((tempFingerInfo.x - calibMinX) * calibFactorX);
								fingerInfos[index].y = (int)
										((tempFingerInfo.y - calibMinY) * calibFactorY);

//								fingerInfos[index].x = ((tempFingerInfo.x
//										- calibMinX) * displayWidth)
//										/ (calibMaxX - calibMinX);
//								fingerInfos[index].y = ((tempFingerInfo.y
//										- calibMinY) * displayHeight)
//										/ (calibMaxY - calibMinY);
							}
							if (calibSwapX)
								fingerInfos[index].x = screenWidth
										- fingerInfos[index].x;
							if (calibSwapY)
								fingerInfos[index].y = screenHeight
										- fingerInfos[index].y;

							if (fingerInfos[index].x < 0)
								fingerInfos[index].x = 0;
							if (fingerInfos[index].y < 0)
								fingerInfos[index].y = 0;
							if (fingerInfos[index].x > screenWidth)
								fingerInfos[index].x = screenWidth;
							if (fingerInfos[index].y > screenHeight)
								fingerInfos[index].y = screenHeight;
						}
						fingerInfos[index].down = 1;
					} else if (0 == ev[i].code) { // Ev_Sync event end
						/* All finger data received, so process now. */
						processFingers();

						/* Reset for next set of events */
						fingerInfos[0].down = 0;
						fingerInfos[1].down = 0;
						fingersDown = 0;
						tempFingerInfo.id = -1;
					}
				} else if (ev[i].type == EV_MSC && (ev[i].code == MSC_RAW
						|| ev[i].code == MSC_SCAN)) {
				} else {
					/* Set finger info values for current finger */
					if (ev[i].code == 57) {
						tempFingerInfo.id = ev[i].value;
					};
					if (ev[i].code == 53) {
						tempFingerInfo.x = ev[i].value;
						fingersDown++;
					};
					if (ev[i].code == 54) {
						tempFingerInfo.y = ev[i].value;
					};
				}
			}
		}

		/* Stream stopped, probably because module has been unloaded */
		close(fileDesc);

		/* Clean up */
		if (active) {
			ungrab(display, deviceID);
		}
		releaseButton();

		/* Wait until device file is there again */
		while ((fileDesc = open(devname, O_RDONLY)) < 0) {
			sleep(1);
		}

	}

}

