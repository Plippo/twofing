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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include "twofingemu.h"
#include "gestures.h"
#include <sys/types.h>
#include <sys/stat.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int debugMode = 0;

int inDebugMode() {
	return debugMode;
}


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
FingerInfo fingerInfos[2] = { { .rawX=0, .rawY=0, .id = -1, .setThisTime = 0 }, { .rawX=0, .rawY=0,
		.id = -1, .setThisTime = 0 } };

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

/* Two finger gestures activated (and input grabbed)? */
int active = 0;
/* Activate two finger gestures (and grab input) as soon as the fingers have been removed */
int activateAtRelease = 0;

/* Finger data */
int fingersDown = 0;
int fingersWereDown = 0;
/* Has button press of first button been called in XTest? */
int buttonDown = 0;



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

Time getCurrentTime() {
	return fairlyCurrentTime;
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
/* Send an XTest event to press the first button if it is not pressed yet */
void pressButton() {
	if(!buttonDown) {
		buttonDown = 1;
		XTestFakeButtonEvent(display, 1, True, CurrentTime);
		XFlush(display);
	}
}
/* Is the first button currently pressed? */
int isButtonDown() {
	return buttonDown;
}


/* Moves the pointer to the given position */
void movePointer(int x, int y) {
	/* Move pointer to center between touch points */
	XTestFakeMotionEvent(display, -1, x, y, CurrentTime);
	XFlush(display);
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
				XFlush(display);
			}
			if (action->modifier & MODIFIER_CONTROL) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display,
						XK_Control_L), True, CurrentTime);
				XFlush(display);
			}
			if (action->modifier & MODIFIER_ALT) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_Alt_L),
						True, CurrentTime);
				XFlush(display);
			}
			if (action->modifier & MODIFIER_SUPER) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Super_L), True,
						CurrentTime);
				XFlush(display);
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
				XFlush(display);
			}
			if (action->modifier & MODIFIER_CONTROL) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display,
						XK_Control_L), False, CurrentTime);
				XFlush(display);
			}
			if (action->modifier & MODIFIER_ALT) {
				XTestFakeKeyEvent(display, XKeysymToKeycode(display, XK_Alt_L),
						False, CurrentTime);
				XFlush(display);
			}
			if (action->modifier & MODIFIER_SUPER) {
				XTestFakeKeyEvent(display,
						XKeysymToKeycode(display, XK_Super_L), False,
						CurrentTime);
				XFlush(display);
			}
		}
	}

}

Window getParentWindow(Window w) {
	Window root, parent;
	Window* childWindows = NULL;
	unsigned int childCount;
	if(XQueryTree(display, w, &root, &parent, &childWindows, &childCount)) {

		if (childWindows != NULL)
			XFree(childWindows);
		return parent;
	} else {
		return None;
	}

}

Window getLastChildWindow(Window w) {
	Window root, parent;
	Window* childWindows = NULL;
	unsigned int childCount;
	if(XQueryTree(display, w, &root, &parent, &childWindows, &childCount)) {

		if (childWindows != NULL) {
			if(childCount > 0) {
				printf("%i children.\n", childCount);
				Window child = childWindows[childCount - 1];
				XFree(childWindows);
				return child;
			}
			XFree(childWindows);
		}
		return None;
	} else {
		return None;
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
			Window parent = getParentWindow(currentWindow);

			if(parent == None || currentWindow == parent) {
				/* something wrong */
				XFree(classHint);
				return currentWindow;
			}
			/* Continue with parent until we find WM_CLASS */
			currentWindow = parent;
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


/* Sets the calibrated x, y coordinates from the raw coordinates in the given FingerInfo */
void calibrate(FingerInfo* fingerInfo) {
	if (calibSwapAxes) {

		fingerInfo->x = (int)
				((fingerInfo->rawY - calibMinX) * calibFactorX);
		fingerInfo->y = (int)
				((fingerInfo->rawX - calibMinY) * calibFactorY);

	} else {

		fingerInfo->x = (int)
				((fingerInfo->rawX - calibMinX) * calibFactorX);
		fingerInfo->y = (int)
				((fingerInfo->rawY - calibMinY) * calibFactorY);
	}
	if (calibSwapX)
		fingerInfo->x = screenWidth
				- fingerInfo->x;
	if (calibSwapY)
		fingerInfo->y = screenHeight
				- fingerInfo->y;

	if (fingerInfo->x < 0)
		fingerInfo->x = 0;
	if (fingerInfo->y < 0)
		fingerInfo->y = 0;
	if (fingerInfo->x > screenWidth)
		fingerInfo->x = screenWidth;
	if (fingerInfo->y > screenHeight)
		fingerInfo->y = screenHeight;
}

/* Process the finger data gathered from the last set of events */
void processFingers() {
	int i;
	fingersDown = 0;
	for(i = 0; i < 2; i++) {
		if(fingerInfos[i].id != -1) {
			calibrate(&(fingerInfos[i]));
			fingersDown++;
		}
	}

	/* If we should activate at release, do it. */
	if (buttonDown == 0 && fingersDown == 0 && activateAtRelease != 0) {
		releaseButton();
		activate();
	}

	if (active == 0)
		return;

	processFingerGesture(fingerInfos, fingersDown, fingersWereDown);

	/* Save number of fingers to compare next time */
	fingersWereDown = fingersDown;

	/* If we should activate at release, do it (now again because fingersDown might have changed). */
	if (buttonDown == 0 && fingersDown == 0 && activateAtRelease != 0) {
		releaseButton();
		activate();
	}
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

/* Returns a pointer to the profile of the currently selected
 * window, or defaultProfile if there is no specific profile for it or the window is invalid. */
char* getWindowClass(Window w) {
	char * result = NULL;
	if (w != None) {

		XClassHint* classHint = XAllocClassHint();

		/* Read WM_CLASS member */
		if (XGetClassHint(display, w, classHint)) {

			if(classHint->res_class != NULL) {

				result = malloc((strlen(classHint->res_name) + 1) * sizeof(char));
				if(result != NULL) {
					strcpy(result, classHint->res_name);
				}
				XFree(classHint->res_class);

			}

			if(classHint->res_name != NULL) XFree(classHint->res_name);
			XFree(classHint);
		}
	}

	return result;

}


/* Returns whether the given window is blacklisted */
int isWindowBlacklisted(Window w) {
	if(w == None) return 0;

	return isWindowBlacklistedForGestures(w);
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
	int clickMode = 2;

	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			doDaemonize = 0;
			debugMode = 1;
		} else if (strcmp(argv[i], "--wait") == 0) {
			doWait = 1;
		} else if (strcmp(argv[i], "--click=first") == 0) {
			clickMode = 0;
		} else if (strcmp(argv[i], "--click=second") == 0) {
			clickMode = 1;
		} else if (strcmp(argv[i], "--click=center") == 0) {
			clickMode = 2;
		} else {
			devname = argv[i];
		}

	}

	initGestures(clickMode);



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

		/* If set, twofing assumes that the device uses the multitouch slot protocol.
		   Set to 1 the first time a ABS_MT_SLOT event occurs. Set to 0 the first time a MT_SYNC event occurs.*/
		int useSlots = 1;
		int currentSlot = 0;
		/* If we don't use the slot protocol, we collect all data of one finger into tempFingerInfo and set
		   it to the correct slot once MT_SYNC occurs. */
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

						if(useSlots) {
							/* This messsage indicates we don't use the slot protocol. So
							   set useSlots and ignore data for now. */
							useSlots = 0;
							currentSlot = -1;
							if(debugMode) printf("Switch to non-slot protocol.\n");
						} else {

							/* Look for slot to put the data into by looking at the tracking ids */
							int index = -1;
							int i;
							for(i = 0; i < 2; i++) {
								if(fingerInfos[i].id == tempFingerInfo.id) {
									index = i;
									break;
								}
							}
							
							if(index == -1) {
								for(i = 0; i < 2; i++) {
									if(fingerInfos[i].id == -1) {
										/* "Empty" slot, so we can add it. */
										index = i;
										fingerInfos[i].id = tempFingerInfo.id;
										break;
									}
								}
							}

							if(index != -1) {
								fingerInfos[index].setThisTime = 1;
								fingerInfos[index].rawX = tempFingerInfo.rawX;
								fingerInfos[index].rawY = tempFingerInfo.rawY;
							}
						}
					} else if (0 == ev[i].code) { // Ev_Sync event end

						if(!useSlots) {
							/* Clear slots not set this time */
							int i;
							for(i = 0; i < 2; i++) {
								if(fingerInfos[i].setThisTime) {
									fingerInfos[i].setThisTime = 0;
								} else {
									/* Clear slot */
									fingerInfos[i].id = -1;
								}
							}
						}


						/* All finger data received, so process now. */
						processFingers();

						if(!useSlots) {
							/* If we don't use the slot protocol, reset for next set of events */
							tempFingerInfo.id = -1;
						}
					}
				} else if (ev[i].type == EV_MSC && (ev[i].code == MSC_RAW
						|| ev[i].code == MSC_SCAN)) {

				} else if (ev[i].code == 47) {
					/* Slot event */
					if(!useSlots) {
						useSlots = 1;
						if(debugMode) printf("Switch to slots protocol.\n");
					}
					currentSlot = ev[i].value;
					if(currentSlot < 0 || currentSlot > 1) currentSlot = -1;
				} else {
					/* Set finger info values for current finger */
					if (ev[i].code == 57) {
						/* ABS_MT_TRACKING_ID */
						if(useSlots) {
							if(currentSlot != -1) {
								fingerInfos[currentSlot].id = ev[i].value;
							}
						} else {
							tempFingerInfo.id = ev[i].value;
						}
					};
					if (ev[i].code == 53) {
						if(useSlots) {
							if(currentSlot != -1) {
								fingerInfos[currentSlot].rawX = ev[i].value;
							}
						} else {
							tempFingerInfo.rawX = ev[i].value;
						}
					};
					if (ev[i].code == 54) {
						if(useSlots) {
							if(currentSlot != -1) {
								fingerInfos[currentSlot].rawY = ev[i].value;
							}
						} else {
							tempFingerInfo.rawY = ev[i].value;
						}
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

