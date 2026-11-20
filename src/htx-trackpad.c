#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	VENDOR_ID = 0x048D,
	PRODUCT_ID = 0x8911,
	REPORT_ID_INPUT_MODE = 3,
	REPORT_ID_TOUCH = 4,
	INPUT_MODE_TOUCHPAD = 3,
	TOUCH_REPORT_LENGTH = 30,
	MAX_CONTACTS = 5,
	CONTACT_LENGTH = 5,
	CONTACT_COUNT_OFFSET = 28,
	// Keeps the pad's touch identities clear of the built-in trackpad's own, which they share a device with.
	TOUCH_INDEX_BASE = 16,
	KEY_CODE_LEFT_ARROW = 0x7B,
	KEY_CODE_RIGHT_ARROW = 0x7C,
	KEY_CODE_DOWN_ARROW = 0x7D,
	KEY_CODE_UP_ARROW = 0x7E,
	SCROLL_HISTORY_LENGTH = 16,
	// Undocumented CGEvent type that carries Dock gestures such as swipes between Spaces.
	CG_EVENT_TYPE_DOCK_SWIPE = 30,
	// Undocumented CGEvent type and fields that carry trackpad gestures such as pinches; AppKit reports field 113 as the magnification.
	CG_EVENT_TYPE_GESTURE = 29,
	CG_EVENT_FIELD_GESTURE_SUBTYPE = 110,
	CG_EVENT_FIELD_GESTURE_ZOOM_VALUE = 113,
	CG_EVENT_FIELD_GESTURE_PHASE = 132,
	// Undocumented scroll field that AppKit reports as isDirectionInvertedFromDevice, which the built-in trackpad sets under natural scrolling.
	CG_EVENT_FIELD_SCROLL_DIRECTION_INVERTED = 137,
};

// From Apple's open-source IOHIDFamily (IOHIDEventTypes.h, IOHIDEventFieldDefs.h); the SDK doesn't ship them.
enum {
	HID_EVENT_TYPE_VENDOR_DEFINED = 1,
	HID_EVENT_TYPE_ZOOM = 8,
	HID_EVENT_TYPE_VELOCITY = 9,
	HID_EVENT_TYPE_DIGITIZER = 11,
	HID_EVENT_TYPE_DOCK_SWIPE = 23,
	HID_FIELD_VELOCITY_X = HID_EVENT_TYPE_VELOCITY << 16,
	HID_FIELD_VELOCITY_Y = HID_EVENT_TYPE_VELOCITY << 16 | 1,
	HID_FIELD_DOCK_SWIPE_MOTION = HID_EVENT_TYPE_DOCK_SWIPE << 16 | 1,
	HID_FIELD_DOCK_SWIPE_PROGRESS = HID_EVENT_TYPE_DOCK_SWIPE << 16 | 2,
	HID_FIELD_DOCK_SWIPE_FLAVOR = HID_EVENT_TYPE_DOCK_SWIPE << 16 | 5,
	HID_GESTURE_MOTION_HORIZONTAL = 1,
	HID_GESTURE_MOTION_VERTICAL = 2,
	HID_GESTURE_FLAVOR_DOCK_PRIMARY = 3,
	HID_PHASE_BEGAN = 1 << 0,
	HID_PHASE_CHANGED = 1 << 1,
	HID_PHASE_ENDED = 1 << 2,
	HID_PHASE_CANCELLED = 1 << 3,
	HID_EVENT_PHASE_SHIFT = 24,
	HID_DIGITIZER_TRANSDUCER_HAND = 3,
	HID_DIGITIZER_EVENT_RANGE = 1 << 0,
	HID_DIGITIZER_EVENT_TOUCH = 1 << 1,
	HID_DIGITIZER_EVENT_POSITION = 1 << 2,
	HID_DIGITIZER_EVENT_IDENTITY = 1 << 5,
	HID_DIGITIZER_EVENT_ATTRIBUTE = 1 << 6,
};

// What AppKit needs to read a digitizer event as trackpad touches, found by disassembling AppKit and MultitouchSupport (_mthid_isPathCollection).
enum {
	// The hand event must be flagged as a collection of paths.
	HID_FIELD_DIGITIZER_COLLECTION = HID_EVENT_TYPE_DIGITIZER << 16 | 22,
	// A vendor-defined child with this usage and version carries the multitouch device ID in its first 8 bytes.
	MULTITOUCH_PATH_USAGE_PAGE = 0xFF00,
	MULTITOUCH_PATH_USAGE = 0x1777,
	MULTITOUCH_PATH_VERSION = 1,
	MULTITOUCH_PATH_DATA_LENGTH = 40,
	// AppKit looks the touches' device up by the IORegistry ID in this CGEvent field.
	CG_EVENT_FIELD_DEVICE_REGISTRY_ID = 87,
};

// Logical ranges 0-1599 and 0-1199 over a physical width of 4.61 in, per the report descriptor; Y has the same density.
static const double LOGICAL_MAX_X = 1599.0;
static const double LOGICAL_MAX_Y = 1199.0;
static const double COUNTS_PER_MM = LOGICAL_MAX_X / 117.1;

static const double TAP_MAX_SECONDS = 0.25;
static const double TAP_MAX_TRAVEL_MM = 2.0;
// Measured on the built-in trackpad with tap-drag on: a tap clicks this long after the lift, unless the finger lands again first.
static const double TAP_DRAG_WINDOW_SECONDS = 0.23;
static const double DOUBLE_CLICK_SECONDS = 0.5;
static const double DRAG_LOCK_SECONDS = 1.0;
static const double SWIPE_MIN_TRAVEL_MM = 10.0;
static const double DOCK_SWIPE_LOCK_TRAVEL_MM = 3.0;
static const double DOCK_SWIPE_PROGRESS_PER_MM_HORIZONTAL = 1.0 / 50.0;
static const double DOCK_SWIPE_PROGRESS_PER_MM_VERTICAL = -1.0 / 40.0;
static const double TRACKING_MULTIPLIER_AT_ZERO = 0.25;
static const double SCROLL_START_TRAVEL_MM = 1.0;
static const double SCROLL_FLOOR_MIN_COUNTS = 0.75;
static const double PINCH_START_SPREAD_MM = 2.0;
static const double PINCH_MAGNIFICATION_PER_MM = 0.0315;
static const double MOMENTUM_EXIT_WINDOW_SECONDS = 0.05;
static const double MOMENTUM_BOOST = 2.4;
static const double MOMENTUM_TIME_CONSTANT_SECONDS = 0.224;
static const double MOMENTUM_STOP_SPEED = 100.0;
static const double MOMENTUM_FRAME_SECONDS = 1.0 / 120.0;
static const double MODE_REFRESH_SECONDS = 10.0;

typedef struct {
	double speed;
	double gain;
} CurvePoint;

// Median cursor points per mm of finger travel, by finger speed in mm/s, measured on the built-in trackpad of a 14-inch MacBook Pro (Mac16,8) at Tracking speed 1.0.
static const CurvePoint POINTER_CURVE[] = {
	{0, 3.0},
	{30, 6.4},
	{50, 9.7},
	{70, 10.6},
	{90, 12.3},
	{115, 16.3},
	{145, 17.3},
	{180, 22.5},
	{225, 27.8},
	{275, 32.9},
	{350, 39.2},
	{450, 49.0},
	{600, 52.9},
	{850, 60.8},
};

// Scroll points per mm of finger travel, by finger speed in mm/s, measured on the same trackpad at the default scrolling speed.
static const CurvePoint SCROLL_CURVE[] = {
	{0, 6.0},
	{65, 6.0},
	{90, 6.4},
	{115, 7.0},
	{150, 7.8},
	{190, 8.6},
	{250, 9.8},
	{300, 11.2},
};

// One finger, with its position normalized to the pad's surface from the top left.
typedef struct {
	int contactId;
	double x;
	double y;
} Contact;

typedef struct {
	int count;
	unsigned idMask;
	double x;
	double y;
	double spread;
	Contact contacts[MAX_CONTACTS];
} Touch;

typedef enum {
	TWO_FINGER_UNDECIDED,
	TWO_FINGER_SCROLL,
	TWO_FINGER_PINCH,
} TwoFingerGesture;

typedef struct {
	double time;
	int32_t deltaX;
	int32_t deltaY;
} ScrollSample;

typedef struct {
	bool touching;
	double touchStart;
	int maxFingers;
	double travel;
	double smoothedSpeed;
	double peakSpeed;
	double pointerFingerTravel;
	double pointerCursorTravel;
	bool pressedAtLanding;
	bool dragging;
	bool referenceValid;
	unsigned referenceMask;
	double referenceX;
	double referenceY;
	double referenceSpread;
	double referenceTime;
	double cursorX;
	double cursorY;
	double lastTapEnd;
	int clickCount;
	double scrollRemainderX;
	double scrollRemainderY;
	TwoFingerGesture twoFingerGesture;
	double twoFingerX;
	double twoFingerY;
	double twoFingerSpread;
	double pinchMagnification;
	bool scrollBegan;
	ScrollSample scrollHistory[SCROLL_HISTORY_LENGTH];
	int scrollHistoryNext;
	double swipeX;
	double swipeY;
	bool swiped;
	int swipeMotion;
	double swipeProgress;
	double swipeVelocity;
	bool naturalScrolling;
	bool momentumScrolling;
	bool pinchZooming;
	bool tapToDrag;
	double trackingMultiplier;
} Gesture;

// Coasting after a two-finger scroll outlives the touch that started it.
typedef struct {
	CFRunLoopTimerRef timer;
	double start;
	double velocityX;
	double velocityY;
	int32_t postedX;
	int32_t postedY;
	bool began;
} Momentum;

typedef struct {
	const char *direction;
	CGKeyCode keyCode;
	const char *action;
} Swipe;

typedef struct __IOHIDEvent *IOHIDEventRef;

// Private IOKit and SkyLight functions that build the gesture events Apple's trackpads send; none of them is in the SDK.
typedef struct {
	IOHIDEventRef (*create)(CFAllocatorRef allocator, uint32_t type, uint64_t timestamp, uint32_t options);
	void (*setEventFlags)(IOHIDEventRef event, uint32_t flags);
	void (*setIntegerValue)(IOHIDEventRef event, uint32_t field, CFIndex value);
	void (*setDoubleValue)(IOHIDEventRef event, uint32_t field, double value);
	void (*appendEvent)(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
	void (*attachToEvent)(CGEventRef event, IOHIDEventRef hidEvent);
} DockSwipeApi;

// Private IOKit and SkyLight functions that build the touch events Apple's trackpads send for every finger.
typedef struct {
	IOHIDEventRef (*createDigitizer)(CFAllocatorRef allocator, uint64_t timestamp, uint32_t transducerType, uint32_t index, uint32_t identity, uint32_t eventMask, uint32_t buttonMask, double x, double y, double z, double tipPressure, double barrelPressure, Boolean range, Boolean touch, uint32_t options);
	IOHIDEventRef (*createFinger)(CFAllocatorRef allocator, uint64_t timestamp, uint32_t index, uint32_t identity, uint32_t eventMask, double x, double y, double z, double tipPressure, double twist, Boolean range, Boolean touch, uint32_t options);
	IOHIDEventRef (*createVendorDefined)(CFAllocatorRef allocator, uint64_t timestamp, uint32_t usagePage, uint32_t usage, uint32_t version, const uint8_t *data, CFIndex length, uint32_t options);
	void (*setSenderID)(IOHIDEventRef event, uint64_t senderID);
	void (*setIntegerValue)(IOHIDEventRef event, uint32_t field, CFIndex value);
	void (*appendEvent)(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
	void (*attachToEvent)(CGEventRef event, IOHIDEventRef hidEvent);
} TouchApi;

static Gesture gesture;
static Momentum momentum;
static DockSwipeApi dockSwipeApi;
static bool dockSwipesAvailable;
static TouchApi touchApi;
static bool touchEventsAvailable;
static uint64_t multitouchRegistryId;
static uint64_t multitouchDeviceId;
static Contact postedContacts[MAX_CONTACTS];
static int postedContactCount;
static CGEventSourceRef eventSource;
static CFRunLoopTimerRef dragReleaseTimer;
static CFRunLoopTimerRef tapClickTimer;
static IOHIDDeviceRef touchpad;
static double speedFactor = 1.0;
static bool verbose;
static bool accessibilityTrusted;
static bool inputMonitoringGranted;
static double loggedTrackingScaling = -1;

static double now(void) {
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);

	return spec.tv_sec + spec.tv_nsec / 1e9;
}

static void logArguments(const char *format, va_list arguments) {
	char stamp[32];
	time_t seconds = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&seconds));
	printf("%s ", stamp);
	vprintf(format, arguments);
	printf("\n");
}

static void logLine(const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	logArguments(format, arguments);
	va_end(arguments);
}

static void logVerbose(const char *format, ...) {
	if (!verbose) {
		return;
	}

	va_list arguments;
	va_start(arguments, format);
	logArguments(format, arguments);
	va_end(arguments);
}

static const char *yesNo(bool value) {
	if (value) {
		return "yes";
	}

	return "no";
}

static bool readNaturalScrolling(void) {
	CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("com.apple.swipescrolldirection"), kCFPreferencesAnyApplication);
	bool natural = true;
	if (value) {
		if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
			natural = CFBooleanGetValue(value);
		}

		CFRelease(value);
	}

	return natural;
}

// On/off trackpad options from System Settings, such as scrolling inertia (TrackpadMomentumScroll) and pinch to zoom (TrackpadPinch).
static bool readTrackpadOption(CFStringRef key) {
	CFPropertyListRef value = CFPreferencesCopyAppValue(key, CFSTR("com.apple.AppleMultitouchTrackpad"));
	bool enabled = true;
	if (value) {
		if (CFGetTypeID(value) == CFNumberGetTypeID()) {
			int number = 1;
			CFNumberGetValue(value, kCFNumberIntType, &number);
			enabled = number != 0;
		} else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
			enabled = CFBooleanGetValue(value);
		}

		CFRelease(value);
	}

	return enabled;
}

// Maps the Trackpad "Tracking speed" slider (0 to 3) linearly onto a pointer multiplier that is 1 at the slider's 1.0 notch.
static double readTrackingMultiplier(void) {
	CFPropertyListRef value = CFPreferencesCopyAppValue(CFSTR("com.apple.trackpad.scaling"), kCFPreferencesAnyApplication);
	double scaling = 1.0;
	if (value) {
		if (CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &scaling);
		}

		CFRelease(value);
	}

	double multiplier = TRACKING_MULTIPLIER_AT_ZERO + (1.0 - TRACKING_MULTIPLIER_AT_ZERO) * scaling;
	if (scaling != loggedTrackingScaling) {
		loggedTrackingScaling = scaling;
		logLine("tracking speed: %.4f (pointer multiplier %.3f)", scaling, multiplier);
	}

	return multiplier;
}

static Touch parseTouch(const uint8_t *report) {
	Touch touch = {0};
	int slots = report[CONTACT_COUNT_OFFSET];
	if (slots > MAX_CONTACTS) {
		slots = MAX_CONTACTS;
	}

	double xs[MAX_CONTACTS];
	double ys[MAX_CONTACTS];
	for (int i = 0; i < slots; i++) {
		const uint8_t *contact = report + 1 + i * CONTACT_LENGTH;
		bool confident = contact[0] & 0x01;
		bool tipDown = contact[0] & 0x02;
		if (!confident || !tipDown) {
			continue;
		}

		int contactId = (contact[0] >> 2) & 0x07;
		int rawX = contact[1] | contact[2] << 8;
		int rawY = contact[3] | contact[4] << 8;
		xs[touch.count] = rawX / COUNTS_PER_MM;
		ys[touch.count] = rawY / COUNTS_PER_MM;
		touch.x += xs[touch.count];
		touch.y += ys[touch.count];
		touch.contacts[touch.count] = (Contact){contactId, rawX / LOGICAL_MAX_X, rawY / LOGICAL_MAX_Y};
		touch.idMask |= 1u << contactId;
		touch.count++;
	}

	if (touch.count > 0) {
		touch.x /= touch.count;
		touch.y /= touch.count;
	}

	if (touch.count == 2) {
		touch.spread = hypot(xs[0] - xs[1], ys[0] - ys[1]);
	}

	return touch;
}

static void syncCursor(void) {
	CGEventRef event = CGEventCreate(NULL);
	CGPoint location = CGEventGetLocation(event);
	CFRelease(event);
	gesture.cursorX = location.x;
	gesture.cursorY = location.y;
}

static CGPoint clampToDisplays(CGPoint from, CGPoint to) {
	CGDirectDisplayID display;
	uint32_t count = 0;
	CGGetDisplaysWithPoint(to, 1, &display, &count);
	if (count > 0) {
		return to;
	}

	CGGetDisplaysWithPoint(from, 1, &display, &count);
	if (count == 0) {
		display = CGMainDisplayID();
	}

	CGRect bounds = CGDisplayBounds(display);
	to.x = fmin(fmax(to.x, CGRectGetMinX(bounds)), CGRectGetMaxX(bounds) - 1);
	to.y = fmin(fmax(to.y, CGRectGetMinY(bounds)), CGRectGetMaxY(bounds) - 1);

	return to;
}

static void postMouse(CGEventType type, CGMouseButton button, int64_t clickState, int64_t deltaX, int64_t deltaY) {
	CGEventRef event = CGEventCreateMouseEvent(eventSource, type, CGPointMake(gesture.cursorX, gesture.cursorY), button);
	if (!event) {
		return;
	}

	CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickState);
	CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, deltaX);
	CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, deltaY);
	CGEventPost(kCGHIDEventTap, event);
	CFRelease(event);
}

static void click(CGMouseButton button, int64_t clickState) {
	CGEventType downType = kCGEventOtherMouseDown;
	CGEventType upType = kCGEventOtherMouseUp;
	if (button == kCGMouseButtonLeft) {
		downType = kCGEventLeftMouseDown;
		upType = kCGEventLeftMouseUp;
	} else if (button == kCGMouseButtonRight) {
		downType = kCGEventRightMouseDown;
		upType = kCGEventRightMouseUp;
	}

	postMouse(downType, button, clickState, 0, 0);
	postMouse(upType, button, clickState, 0, 0);
}

static void postKey(CGKeyCode keyCode, bool keyDown, CGEventFlags flags) {
	CGEventRef event = CGEventCreateKeyboardEvent(eventSource, keyCode, keyDown);
	if (!event) {
		return;
	}

	CGEventSetFlags(event, flags);
	CGEventPost(kCGHIDEventTap, event);
	CFRelease(event);
}

// The default Mission Control shortcuts are registered with the Fn flag that arrow keys carry.
static void pressControlArrow(CGKeyCode keyCode) {
	CGEventFlags flags = kCGEventFlagMaskControl | kCGEventFlagMaskSecondaryFn;
	postKey(keyCode, true, flags);
	postKey(keyCode, false, flags);
}

static double curveGain(const CurvePoint *curve, size_t count, double speed) {
	if (speed <= curve[0].speed) {
		return curve[0].gain;
	}

	for (size_t i = 1; i < count; i++) {
		if (speed <= curve[i].speed) {
			CurvePoint low = curve[i - 1];
			CurvePoint high = curve[i];

			return low.gain + (high.gain - low.gain) * (speed - low.speed) / (high.speed - low.speed);
		}
	}

	return curve[count - 1].gain;
}

static double pointerGain(double speed) {
	return curveGain(POINTER_CURVE, sizeof(POINTER_CURVE) / sizeof(POINTER_CURVE[0]), speed);
}

static double scrollGain(double speed) {
	return curveGain(SCROLL_CURVE, sizeof(SCROLL_CURVE) / sizeof(SCROLL_CURVE[0]), speed);
}

static void moveCursor(double dxMm, double dyMm, double seconds) {
	double speed = hypot(dxMm, dyMm) / seconds;
	gesture.smoothedSpeed = 0.7 * gesture.smoothedSpeed + 0.3 * speed;
	double scale = pointerGain(gesture.smoothedSpeed) * speedFactor * gesture.trackingMultiplier;
	CGPoint from = CGPointMake(gesture.cursorX, gesture.cursorY);
	CGPoint to = clampToDisplays(from, CGPointMake(from.x + dxMm * scale, from.y + dyMm * scale));
	int64_t deltaX = llround(to.x) - llround(from.x);
	int64_t deltaY = llround(to.y) - llround(from.y);
	gesture.cursorX = to.x;
	gesture.cursorY = to.y;
	gesture.peakSpeed = fmax(gesture.peakSpeed, gesture.smoothedSpeed);
	gesture.pointerFingerTravel += hypot(dxMm, dyMm);
	gesture.pointerCursorTravel += hypot(to.x - from.x, to.y - from.y);
	if (gesture.dragging) {
		postMouse(kCGEventLeftMouseDragged, kCGMouseButtonLeft, 1, deltaX, deltaY);
	} else {
		postMouse(kCGEventMouseMoved, kCGMouseButtonLeft, 0, deltaX, deltaY);
	}
}

// Apple's trackpads tag each scroll with a gesture phase and keep sending momentum-phase events after a lift; apps such as Kitty and JetBrains IDEs coast only on those.
static void postScroll(int32_t deltaX, int32_t deltaY, int64_t scrollPhase, int64_t momentumPhase) {
	CGEventRef event = CGEventCreateScrollWheelEvent2(eventSource, kCGScrollEventUnitPixel, 2, deltaY, deltaX, 0);
	if (!event) {
		return;
	}

	CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
	CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, scrollPhase);
	CGEventSetIntegerValueField(event, kCGScrollWheelEventMomentumPhase, momentumPhase);
	CGEventSetIntegerValueField(event, CG_EVENT_FIELD_SCROLL_DIRECTION_INVERTED, gesture.naturalScrolling);
	CGEventPost(kCGHIDEventTap, event);
	CFRelease(event);
}

static void stopMomentum(const char *reason) {
	if (!momentum.timer) {
		return;
	}

	CFRunLoopTimerInvalidate(momentum.timer);
	CFRelease(momentum.timer);
	momentum.timer = NULL;
	if (momentum.began) {
		postScroll(0, 0, 0, kCGMomentumScrollPhaseEnd);
	}

	logVerbose("momentum %s after %.2f s: %d, %d pt", reason, now() - momentum.start, momentum.postedX, momentum.postedY);
}

// Measured on Apple's trackpad: coasting speed decays exponentially from a boosted exit speed, so the distance covered is closed-form.
static void onMomentumTimer(CFRunLoopTimerRef timer, void *info) {
	double elapsed = now() - momentum.start;
	double decay = exp(-elapsed / MOMENTUM_TIME_CONSTANT_SECONDS);
	if (hypot(momentum.velocityX, momentum.velocityY) * decay < MOMENTUM_STOP_SPEED) {
		stopMomentum("ended");

		return;
	}

	double reach = MOMENTUM_TIME_CONSTANT_SECONDS * (1.0 - decay);
	int32_t deltaX = (int32_t)trunc(momentum.velocityX * reach) - momentum.postedX;
	int32_t deltaY = (int32_t)trunc(momentum.velocityY * reach) - momentum.postedY;
	if (deltaX == 0 && deltaY == 0) {
		return;
	}

	momentum.postedX += deltaX;
	momentum.postedY += deltaY;
	int64_t phase = kCGMomentumScrollPhaseContinue;
	if (!momentum.began) {
		momentum.began = true;
		phase = kCGMomentumScrollPhaseBegin;
	}

	postScroll(deltaX, deltaY, 0, phase);
}

static void startMomentum(double velocityX, double velocityY) {
	stopMomentum("replaced");
	if (hypot(velocityX, velocityY) < MOMENTUM_STOP_SPEED) {
		return;
	}

	momentum = (Momentum){.start = now(), .velocityX = velocityX, .velocityY = velocityY};
	momentum.timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + MOMENTUM_FRAME_SECONDS, MOMENTUM_FRAME_SECONDS, 0, 0, onMomentumTimer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), momentum.timer, kCFRunLoopDefaultMode);
}

// Apple's trackpad sends at least one point for every frame in which the fingers move along the scroll, which sets the pace of a slow crawl.
static int32_t takeScrollStep(double *remainder, double points, bool floored) {
	*remainder += points;
	int32_t step = (int32_t)trunc(*remainder);
	if (step == 0 && floored) {
		*remainder = 0;

		return (int32_t)copysign(1.0, points);
	}

	*remainder -= step;

	return step;
}

static void scroll(double dxMm, double dyMm, double seconds, double time) {
	double speed = hypot(dxMm, dyMm) / seconds;
	gesture.smoothedSpeed = 0.7 * gesture.smoothedSpeed + 0.3 * speed;
	double scale = scrollGain(gesture.smoothedSpeed);
	if (!gesture.naturalScrolling) {
		scale = -scale;
	}

	bool vertical = fabs(dyMm) >= fabs(dxMm);
	int32_t wheelX = takeScrollStep(&gesture.scrollRemainderX, dxMm * scale, !vertical && fabs(dxMm) * COUNTS_PER_MM >= SCROLL_FLOOR_MIN_COUNTS);
	int32_t wheelY = takeScrollStep(&gesture.scrollRemainderY, dyMm * scale, vertical && fabs(dyMm) * COUNTS_PER_MM >= SCROLL_FLOOR_MIN_COUNTS);
	if (wheelX == 0 && wheelY == 0) {
		return;
	}

	int64_t phase = kCGScrollPhaseChanged;
	if (!gesture.scrollBegan) {
		gesture.scrollBegan = true;
		phase = kCGScrollPhaseBegan;
	}

	postScroll(wheelX, wheelY, phase, kCGMomentumScrollPhaseNone);
	gesture.scrollHistory[gesture.scrollHistoryNext] = (ScrollSample){time, wheelX, wheelY};
	gesture.scrollHistoryNext = (gesture.scrollHistoryNext + 1) % SCROLL_HISTORY_LENGTH;
}

// Momentum starts from the average scroll speed over the last moments before the fingers stopped scrolling, as Apple's does.
static void finishScroll(double time, bool coast) {
	if (!gesture.scrollBegan) {
		return;
	}

	gesture.scrollBegan = false;
	postScroll(0, 0, kCGScrollPhaseEnded, kCGMomentumScrollPhaseNone);
	double exitX = 0;
	double exitY = 0;
	for (int i = 0; i < SCROLL_HISTORY_LENGTH; i++) {
		ScrollSample sample = gesture.scrollHistory[i];
		if (time - sample.time < MOMENTUM_EXIT_WINDOW_SECONDS) {
			exitX += sample.deltaX / MOMENTUM_EXIT_WINDOW_SECONDS;
			exitY += sample.deltaY / MOMENTUM_EXIT_WINDOW_SECONDS;
		}
	}

	logVerbose("scroll end: exit velocity %.0f, %.0f pt/s", exitX, exitY);
	if (coast && gesture.momentumScrolling) {
		startMomentum(exitX * MOMENTUM_BOOST, exitY * MOMENTUM_BOOST);
	}
}

static void postPinch(uint32_t phase, double magnification) {
	CGEventRef event = CGEventCreate(NULL);
	if (!event) {
		return;
	}

	CGEventSetType(event, (CGEventType)CG_EVENT_TYPE_GESTURE);
	CGEventSetIntegerValueField(event, CG_EVENT_FIELD_GESTURE_SUBTYPE, HID_EVENT_TYPE_ZOOM);
	CGEventSetIntegerValueField(event, CG_EVENT_FIELD_GESTURE_PHASE, phase);
	CGEventSetDoubleValueField(event, CG_EVENT_FIELD_GESTURE_ZOOM_VALUE, magnification);
	CGEventPost(kCGHIDEventTap, event);
	CFRelease(event);
}

// As measured on Apple's trackpad, magnification follows the change in distance between the fingers linearly, at any speed.
static void pinch(double spreadMm) {
	if (spreadMm == 0) {
		return;
	}

	double magnification = spreadMm * PINCH_MAGNIFICATION_PER_MM;
	gesture.pinchMagnification += magnification;
	postPinch(HID_PHASE_CHANGED, magnification);
}

static void finishPinch(void) {
	if (gesture.twoFingerGesture != TWO_FINGER_PINCH) {
		return;
	}

	gesture.twoFingerGesture = TWO_FINGER_UNDECIDED;
	gesture.twoFingerX = 0;
	gesture.twoFingerY = 0;
	gesture.twoFingerSpread = 0;
	postPinch(HID_PHASE_ENDED, 0);
	logVerbose("pinch end: magnification %+.3f", gesture.pinchMagnification);
}

// Like Apple's trackpad, a two-finger touch waits for a little movement before committing: fingers sliding together scroll, while fingers spreading or closing pinch, even with one finger still, which moves their midpoint half as far.
static void trackTwoFingers(double dxMm, double dyMm, double spreadMm, double seconds, double time) {
	switch (gesture.twoFingerGesture) {
		case TWO_FINGER_SCROLL:
			scroll(dxMm, dyMm, seconds, time);

			return;
		case TWO_FINGER_PINCH:
			pinch(spreadMm);

			return;
		case TWO_FINGER_UNDECIDED:
			break;
	}

	gesture.twoFingerX += dxMm;
	gesture.twoFingerY += dyMm;
	gesture.twoFingerSpread += spreadMm;
	double travel = hypot(gesture.twoFingerX, gesture.twoFingerY);
	double spread = fabs(gesture.twoFingerSpread);
	if (gesture.pinchZooming && spread >= PINCH_START_SPREAD_MM && spread >= travel) {
		gesture.twoFingerGesture = TWO_FINGER_PINCH;
		gesture.pinchMagnification = 0;
		postPinch(HID_PHASE_BEGAN, 0);
		logVerbose("pinch began: spread %+.1f mm, travel %.1f mm", gesture.twoFingerSpread, travel);
	} else if (travel >= SCROLL_START_TRAVEL_MM && (travel > spread || !gesture.pinchZooming)) {
		gesture.twoFingerGesture = TWO_FINGER_SCROLL;
	}
}

// Pad Y grows toward the user, and Spaces follow the fingers as on Apple's trackpads: swiping left brings in the Space on the right.
static Swipe classifySwipe(double dxMm, double dyMm) {
	if (fabs(dxMm) > fabs(dyMm)) {
		if (dxMm < 0) {
			return (Swipe){"left", KEY_CODE_RIGHT_ARROW, "Space on the right"};
		}

		return (Swipe){"right", KEY_CODE_LEFT_ARROW, "Space on the left"};
	}

	if (dyMm < 0) {
		return (Swipe){"up", KEY_CODE_UP_ARROW, "Mission Control"};
	}

	return (Swipe){"down", KEY_CODE_DOWN_ARROW, "App Exposé"};
}

// Without the private gesture API, a swipe falls back to the Mission Control keyboard shortcuts, fired once per touch.
static void triggerShortcutSwipe(double dxMm, double dyMm) {
	if (gesture.swiped) {
		return;
	}

	gesture.swipeX += dxMm;
	gesture.swipeY += dyMm;
	if (hypot(gesture.swipeX, gesture.swipeY) < SWIPE_MIN_TRAVEL_MM) {
		return;
	}

	gesture.swiped = true;
	Swipe swipe = classifySwipe(gesture.swipeX, gesture.swipeY);
	pressControlArrow(swipe.keyCode);
	logVerbose("swipe %s: %s", swipe.direction, swipe.action);
}

static void postDockSwipe(uint32_t phase, double exitVelocity) {
	IOHIDEventRef swipe = dockSwipeApi.create(kCFAllocatorDefault, HID_EVENT_TYPE_DOCK_SWIPE, 0, 0);
	if (!swipe) {
		return;
	}

	dockSwipeApi.setEventFlags(swipe, phase << HID_EVENT_PHASE_SHIFT);
	dockSwipeApi.setIntegerValue(swipe, HID_FIELD_DOCK_SWIPE_MOTION, gesture.swipeMotion);
	dockSwipeApi.setIntegerValue(swipe, HID_FIELD_DOCK_SWIPE_FLAVOR, HID_GESTURE_FLAVOR_DOCK_PRIMARY);
	dockSwipeApi.setDoubleValue(swipe, HID_FIELD_DOCK_SWIPE_PROGRESS, gesture.swipeProgress);
	if (phase == HID_PHASE_ENDED || phase == HID_PHASE_CANCELLED) {
		IOHIDEventRef velocity = dockSwipeApi.create(kCFAllocatorDefault, HID_EVENT_TYPE_VELOCITY, 0, 0);
		if (velocity) {
			dockSwipeApi.setDoubleValue(velocity, HID_FIELD_VELOCITY_X, exitVelocity);
			dockSwipeApi.setDoubleValue(velocity, HID_FIELD_VELOCITY_Y, exitVelocity);
			dockSwipeApi.appendEvent(swipe, velocity, 0);
			CFRelease(velocity);
		}
	}

	CGEventRef event = CGEventCreate(NULL);
	if (event) {
		CGEventSetType(event, (CGEventType)CG_EVENT_TYPE_DOCK_SWIPE);
		dockSwipeApi.attachToEvent(event, swipe);
		CGEventPost(kCGHIDEventTap, event);
		CFRelease(event);
	}

	CFRelease(swipe);
}

static double swipeProgressDelta(double dxMm, double dyMm) {
	if (gesture.swipeMotion == HID_GESTURE_MOTION_HORIZONTAL) {
		return dxMm * DOCK_SWIPE_PROGRESS_PER_MM_HORIZONTAL;
	}

	return dyMm * DOCK_SWIPE_PROGRESS_PER_MM_VERTICAL;
}

// As on Apple's trackpads, the swipe locks to one axis and the Dock follows the fingers, holding wherever they stop.
static void trackDockSwipe(double dxMm, double dyMm, double seconds) {
	if (gesture.swipeMotion == 0) {
		gesture.swipeX += dxMm;
		gesture.swipeY += dyMm;
		if (hypot(gesture.swipeX, gesture.swipeY) <= DOCK_SWIPE_LOCK_TRAVEL_MM) {
			return;
		}

		gesture.swipeMotion = HID_GESTURE_MOTION_VERTICAL;
		if (fabs(gesture.swipeX) > fabs(gesture.swipeY)) {
			gesture.swipeMotion = HID_GESTURE_MOTION_HORIZONTAL;
		}

		gesture.swipeProgress = swipeProgressDelta(gesture.swipeX, gesture.swipeY);
		gesture.swipeVelocity = 0;
		postDockSwipe(HID_PHASE_BEGAN, 0);
		logVerbose("dock swipe began: motion %d, progress %.3f", gesture.swipeMotion, gesture.swipeProgress);

		return;
	}

	double delta = swipeProgressDelta(dxMm, dyMm);
	gesture.swipeVelocity = 0.7 * gesture.swipeVelocity + 0.3 * delta / seconds;
	if (delta == 0) {
		return;
	}

	gesture.swipeProgress += delta;
	postDockSwipe(HID_PHASE_CHANGED, 0);
}

static void finishDockSwipe(void) {
	if (gesture.swipeMotion == 0) {
		return;
	}

	// Lifting while the fingers head back toward where they started snaps the swipe back.
	uint32_t phase = HID_PHASE_ENDED;
	const char *outcome = "ended";
	if (gesture.swipeVelocity * gesture.swipeProgress < 0) {
		phase = HID_PHASE_CANCELLED;
		outcome = "canceled";
	}

	postDockSwipe(phase, gesture.swipeVelocity);
	logVerbose("dock swipe %s: progress %.3f, velocity %.3f/s", outcome, gesture.swipeProgress, gesture.swipeVelocity);
	gesture.swipeMotion = 0;
}

static const Contact *findContact(const Contact *contacts, int count, int contactId) {
	for (int i = 0; i < count; i++) {
		if (contacts[i].contactId == contactId) {
			return &contacts[i];
		}
	}

	return NULL;
}

// Apple's trackpads also send every finger as a touch, and some apps act on those rather than on gestures alone; Chrome's two-finger swipe between pages is one.
static void postTouches(const Touch *touch) {
	if (!touchEventsAvailable || (touch->count == 0 && postedContactCount == 0)) {
		return;
	}

	uint64_t timestamp = mach_absolute_time();
	IOHIDEventRef fingers[MAX_CONTACTS * 2];
	int fingerCount = 0;
	uint32_t handMask = 0;
	double centroidX = 0;
	double centroidY = 0;
	for (int i = 0; i < touch->count; i++) {
		const Contact *contact = &touch->contacts[i];
		const Contact *previous = findContact(postedContacts, postedContactCount, contact->contactId);
		uint32_t mask = HID_DIGITIZER_EVENT_ATTRIBUTE;
		if (!previous) {
			mask |= HID_DIGITIZER_EVENT_RANGE | HID_DIGITIZER_EVENT_TOUCH | HID_DIGITIZER_EVENT_IDENTITY;
		} else if (previous->x != contact->x || previous->y != contact->y) {
			mask = HID_DIGITIZER_EVENT_POSITION;
		}

		fingers[fingerCount++] = touchApi.createFinger(kCFAllocatorDefault, timestamp, TOUCH_INDEX_BASE + contact->contactId, 2 + i, mask, contact->x, contact->y, 0, 0, 0, true, true, 0);
		handMask |= mask;
		centroidX += contact->x / touch->count;
		centroidY += contact->y / touch->count;
	}

	// A lifted finger ends its touch only while still in range, as on Apple's trackpads.
	for (int i = 0; i < postedContactCount; i++) {
		const Contact *contact = &postedContacts[i];
		if (!findContact(touch->contacts, touch->count, contact->contactId)) {
			fingers[fingerCount++] = touchApi.createFinger(kCFAllocatorDefault, timestamp, TOUCH_INDEX_BASE + contact->contactId, 0, HID_DIGITIZER_EVENT_TOUCH, contact->x, contact->y, 0, 0, 0, true, false, 0);
			handMask |= HID_DIGITIZER_EVENT_TOUCH;
		}
	}

	memcpy(postedContacts, touch->contacts, sizeof(Contact) * touch->count);
	postedContactCount = touch->count;
	IOHIDEventRef hand = touchApi.createDigitizer(kCFAllocatorDefault, timestamp, HID_DIGITIZER_TRANSDUCER_HAND, 0, 1, handMask, 0, centroidX, centroidY, 0, 0, 0, true, touch->count > 0, 0);
	uint8_t path[MULTITOUCH_PATH_DATA_LENGTH] = {0};
	memcpy(path, &multitouchDeviceId, sizeof(multitouchDeviceId));
	IOHIDEventRef pathMarker = touchApi.createVendorDefined(kCFAllocatorDefault, timestamp, MULTITOUCH_PATH_USAGE_PAGE, MULTITOUCH_PATH_USAGE, MULTITOUCH_PATH_VERSION, path, sizeof(path), 0);
	CGEventRef event = CGEventCreate(NULL);
	if (hand && pathMarker && event) {
		touchApi.setIntegerValue(hand, HID_FIELD_DIGITIZER_COLLECTION, 1);
		touchApi.setSenderID(hand, multitouchRegistryId);
		for (int i = 0; i < fingerCount; i++) {
			if (fingers[i]) {
				touchApi.appendEvent(hand, fingers[i], 0);
			}
		}

		touchApi.appendEvent(hand, pathMarker, 0);
		CGEventSetType(event, (CGEventType)CG_EVENT_TYPE_GESTURE);
		CGEventSetIntegerValueField(event, CG_EVENT_FIELD_DEVICE_REGISTRY_ID, (int64_t)multitouchRegistryId);
		touchApi.attachToEvent(event, hand);
		CGEventPost(kCGHIDEventTap, event);
	}

	for (int i = 0; i < fingerCount; i++) {
		if (fingers[i]) {
			CFRelease(fingers[i]);
		}
	}

	if (hand) {
		CFRelease(hand);
	}

	if (pathMarker) {
		CFRelease(pathMarker);
	}

	if (event) {
		CFRelease(event);
	}
}

static void cancelDragRelease(void) {
	if (!dragReleaseTimer) {
		return;
	}

	CFRunLoopTimerInvalidate(dragReleaseTimer);
	CFRelease(dragReleaseTimer);
	dragReleaseTimer = NULL;
}

static void releaseDrag(void) {
	cancelDragRelease();
	if (!gesture.dragging) {
		return;
	}

	gesture.dragging = false;
	postMouse(kCGEventLeftMouseUp, kCGMouseButtonLeft, 1, 0, 0);
	logVerbose("drag end");
}

static void onDragReleaseTimer(CFRunLoopTimerRef timer, void *info) {
	releaseDrag();
}

static void scheduleDragRelease(void) {
	cancelDragRelease();
	dragReleaseTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + DRAG_LOCK_SECONDS, 0, 0, 0, onDragReleaseTimer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), dragReleaseTimer, kCFRunLoopDefaultMode);
}

static void cancelTapClick(void) {
	if (!tapClickTimer) {
		return;
	}

	CFRunLoopTimerInvalidate(tapClickTimer);
	CFRelease(tapClickTimer);
	tapClickTimer = NULL;
}

static void postTapClick(void) {
	cancelTapClick();
	click(kCGMouseButtonLeft, gesture.clickCount);
	logVerbose("tap: left click (count %d)", gesture.clickCount);
}

static void onTapClickTimer(CFRunLoopTimerRef timer, void *info) {
	// Another pointing device may have moved the pointer while the click waited.
	syncCursor();
	postTapClick();
}

static void scheduleTapClick(void) {
	cancelTapClick();
	tapClickTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + TAP_DRAG_WINDOW_SECONDS, 0, 0, 0, onTapClickTimer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), tapClickTimer, kCFRunLoopDefaultMode);
}

static void beginTouch(double time) {
	stopMomentum("stopped by a touch");
	gesture.touching = true;
	gesture.touchStart = time;
	gesture.maxFingers = 0;
	gesture.travel = 0;
	gesture.smoothedSpeed = 0;
	gesture.peakSpeed = 0;
	gesture.pointerFingerTravel = 0;
	gesture.pointerCursorTravel = 0;
	gesture.referenceValid = false;
	gesture.scrollRemainderX = 0;
	gesture.scrollRemainderY = 0;
	memset(gesture.scrollHistory, 0, sizeof(gesture.scrollHistory));
	gesture.twoFingerGesture = TWO_FINGER_UNDECIDED;
	gesture.twoFingerX = 0;
	gesture.twoFingerY = 0;
	gesture.twoFingerSpread = 0;
	gesture.swipeX = 0;
	gesture.swipeY = 0;
	gesture.swiped = false;
	gesture.pressedAtLanding = false;
	CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
	CFPreferencesAppSynchronize(CFSTR("com.apple.AppleMultitouchTrackpad"));
	gesture.naturalScrolling = readNaturalScrolling();
	gesture.momentumScrolling = readTrackpadOption(CFSTR("TrackpadMomentumScroll"));
	gesture.pinchZooming = readTrackpadOption(CFSTR("TrackpadPinch"));
	gesture.tapToDrag = readTrackpadOption(CFSTR("Dragging"));
	gesture.trackingMultiplier = readTrackingMultiplier();
	syncCursor();
	if (gesture.dragging) {
		cancelDragRelease();
		logVerbose("drag resumed");
	} else if (tapClickTimer) {
		if (time - gesture.lastTapEnd <= TAP_DRAG_WINDOW_SECONDS) {
			// As on Apple's trackpad, landing again presses at once, so a drag is one press from here and Mission Control never sees a separate click.
			cancelTapClick();
			gesture.dragging = true;
			gesture.pressedAtLanding = true;
			postMouse(kCGEventLeftMouseDown, kCGMouseButtonLeft, gesture.clickCount, 0, 0);
			logVerbose("touched again after a tap: left button down");
		} else {
			postTapClick();
		}
	}
}

static void endTouch(double time) {
	bool isTap = time - gesture.touchStart <= TAP_MAX_SECONDS && gesture.travel <= TAP_MAX_TRAVEL_MM;
	gesture.touching = false;
	gesture.referenceValid = false;
	if (gesture.pointerFingerTravel > TAP_MAX_TRAVEL_MM) {
		logVerbose("pointer: finger %.1f mm, cursor %.0f pt, average %.1f pt/mm, peak speed %.0f mm/s", gesture.pointerFingerTravel, gesture.pointerCursorTravel, gesture.pointerCursorTravel / gesture.pointerFingerTravel, gesture.peakSpeed);
	}

	finishScroll(time, true);
	finishPinch();
	finishDockSwipe();
	// As with "Without Drag Lock", a drag outlives a brief lift so the finger can be re-placed; a tap drops it at once.
	if (gesture.dragging) {
		// A quick second tap releases the press and completes a double click, as on Apple's trackpad.
		if (gesture.pressedAtLanding && isTap) {
			gesture.dragging = false;
			postMouse(kCGEventLeftMouseUp, kCGMouseButtonLeft, gesture.clickCount, 0, 0);
			gesture.clickCount++;
			click(kCGMouseButtonLeft, gesture.clickCount);
			gesture.lastTapEnd = time;
			logVerbose("tap: left click (count %d)", gesture.clickCount);

			return;
		}

		if (isTap) {
			releaseDrag();
		} else {
			scheduleDragRelease();
			logVerbose("drag lifted; held for %.1f s", DRAG_LOCK_SECONDS);
		}

		gesture.lastTapEnd = 0;

		return;
	}

	if (!isTap) {
		gesture.lastTapEnd = 0;

		return;
	}

	switch (gesture.maxFingers) {
		case 1:
			if (time - gesture.lastTapEnd <= DOUBLE_CLICK_SECONDS) {
				gesture.clickCount++;
			} else {
				gesture.clickCount = 1;
			}

			gesture.lastTapEnd = time;
			if (gesture.tapToDrag) {
				scheduleTapClick();
			} else {
				postTapClick();
			}

			break;
		case 2:
			click(kCGMouseButtonRight, 1);
			gesture.lastTapEnd = 0;
			logVerbose("tap: right click");
			break;
		case 3:
			click(kCGMouseButtonCenter, 1);
			gesture.lastTapEnd = 0;
			logVerbose("tap: middle click");
			break;
		default:
			gesture.lastTapEnd = 0;
			break;
	}
}

static void handleTouch(const Touch *touch, double time) {
	postTouches(touch);
	if (touch->count == 0) {
		if (gesture.touching) {
			endTouch(time);
		}

		return;
	}

	if (!gesture.touching) {
		beginTouch(time);
	}

	if (touch->count > gesture.maxFingers) {
		gesture.maxFingers = touch->count;
	}

	// A second finger makes this a two-finger gesture, so the press from landing only completes the tap's click.
	if (gesture.pressedAtLanding && touch->count > 1) {
		gesture.pressedAtLanding = false;
		gesture.dragging = false;
		postMouse(kCGEventLeftMouseUp, kCGMouseButtonLeft, gesture.clickCount, 0, 0);
		logVerbose("tap: left click (count %d)", gesture.clickCount);
	}

	bool twoFingers = touch->count == 2 && gesture.maxFingers == 2;
	if (!twoFingers) {
		finishScroll(time, true);
		finishPinch();
	}

	// Re-anchor whenever the set of fingers changes, so a finger landing or lifting never reads as movement.
	if (!gesture.referenceValid || touch->idMask != gesture.referenceMask) {
		gesture.referenceValid = true;
		gesture.referenceMask = touch->idMask;
		gesture.referenceX = touch->x;
		gesture.referenceY = touch->y;
		gesture.referenceSpread = touch->spread;
		gesture.referenceTime = time;

		return;
	}

	double dx = touch->x - gesture.referenceX;
	double dy = touch->y - gesture.referenceY;
	double spread = touch->spread - gesture.referenceSpread;
	double seconds = fmax(time - gesture.referenceTime, 0.001);
	gesture.referenceX = touch->x;
	gesture.referenceY = touch->y;
	gesture.referenceSpread = touch->spread;
	gesture.referenceTime = time;
	// A quick pinch barely moves the fingers' midpoint, so the spread counts as travel too and keeps it from reading as a tap.
	gesture.travel += hypot(dx, dy) + fabs(spread);

	// A touch acts only at its peak finger count, so lifting out of a scroll never nudges the pointer and lifting out of a swipe never scrolls.
	if (touch->count == 1 && gesture.maxFingers == 1) {
		moveCursor(dx, dy, seconds);
	} else if (twoFingers) {
		trackTwoFingers(dx, dy, spread, seconds, time);
	} else if (touch->count == 3 && gesture.maxFingers == 3) {
		if (dockSwipesAvailable) {
			trackDockSwipe(dx, dy, seconds);
		} else {
			triggerShortcutSwipe(dx, dy);
		}
	}
}

static void resetGesture(void) {
	Touch lifted = {0};
	postTouches(&lifted);
	cancelTapClick();
	releaseDrag();
	finishScroll(now(), false);
	stopMomentum("stopped by a reset");
	finishPinch();
	finishDockSwipe();
	gesture.touching = false;
	gesture.referenceValid = false;
	gesture.lastTapEnd = 0;
}

static IOReturn setTouchpadMode(IOHIDDeviceRef device) {
	uint8_t report[] = {REPORT_ID_INPUT_MODE, INPUT_MODE_TOUCHPAD};

	return IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, REPORT_ID_INPUT_MODE, report, sizeof(report));
}

static void onInputReport(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportId, uint8_t *report, CFIndex length) {
	if (reportId != REPORT_ID_TOUCH || length < TOUCH_REPORT_LENGTH) {
		return;
	}

	Touch touch = parseTouch(report);
	handleTouch(&touch, now());
}

static void onDeviceMatched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
	touchpad = device;
	resetGesture();
	logLine("touchpad connected; switching to touchpad mode: 0x%08x", setTouchpadMode(device));
}

static void onDeviceRemoved(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
	if (device == touchpad) {
		touchpad = NULL;
	}

	resetGesture();
	logLine("touchpad disconnected");
}

static bool checkInputMonitoring(void) {
	return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted;
}

// The firmware sends nothing at all in its power-on mode, so re-assert touchpad mode in case the pad gets reset without being re-enumerated, as can happen around sleep.
static void onRefreshTimer(CFRunLoopTimerRef timer, void *info) {
	bool trusted = AXIsProcessTrusted();
	bool listening = checkInputMonitoring();
	// Exit on a newly granted permission and let launchd's KeepAlive start a fresh process that opens the pad with it.
	if ((trusted && !accessibilityTrusted) || (listening && !inputMonitoringGranted)) {
		logLine("permission granted (accessibility: %s, input monitoring: %s); restarting", yesNo(trusted), yesNo(listening));
		exit(0);
	}

	if (!touchpad || gesture.touching) {
		return;
	}

	IOReturn result = setTouchpadMode(touchpad);
	if (result != kIOReturnSuccess) {
		logLine("re-asserting touchpad mode failed: 0x%08x", result);
	}
}

static bool loadDockSwipeApi(void) {
	void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	void *skylight = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW);
	if (!iokit || !skylight) {
		return false;
	}

	dockSwipeApi.create = dlsym(iokit, "IOHIDEventCreate");
	dockSwipeApi.setEventFlags = dlsym(iokit, "IOHIDEventSetEventFlags");
	dockSwipeApi.setIntegerValue = dlsym(iokit, "IOHIDEventSetIntegerValue");
	dockSwipeApi.setDoubleValue = dlsym(iokit, "IOHIDEventSetDoubleValue");
	dockSwipeApi.appendEvent = dlsym(iokit, "IOHIDEventAppendEvent");
	dockSwipeApi.attachToEvent = dlsym(skylight, "SLEventSetIOHIDEvent");

	return dockSwipeApi.create && dockSwipeApi.setEventFlags && dockSwipeApi.setIntegerValue && dockSwipeApi.setDoubleValue && dockSwipeApi.appendEvent && dockSwipeApi.attachToEvent;
}

// AppKit builds trackpad touches only for a multitouch device it knows, so the pad's touches are filed under the Mac's own trackpad, preferring the built-in one.
static bool findMultitouchDevice(void) {
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleMultitouchDevice"), &iterator) != KERN_SUCCESS) {
		return false;
	}

	bool foundBuiltIn = false;
	for (io_service_t service = IOIteratorNext(iterator); service; service = IOIteratorNext(iterator)) {
		CFTypeRef deviceId = IORegistryEntryCreateCFProperty(service, CFSTR("Multitouch ID"), kCFAllocatorDefault, 0);
		CFTypeRef builtIn = IORegistryEntryCreateCFProperty(service, CFSTR("MT Built-In"), kCFAllocatorDefault, 0);
		uint64_t registryId = 0;
		bool isBuiltIn = builtIn && CFEqual(builtIn, kCFBooleanTrue);
		if (deviceId && CFGetTypeID(deviceId) == CFNumberGetTypeID() && IORegistryEntryGetRegistryEntryID(service, &registryId) == KERN_SUCCESS && (isBuiltIn || !multitouchRegistryId)) {
			CFNumberGetValue(deviceId, kCFNumberSInt64Type, &multitouchDeviceId);
			multitouchRegistryId = registryId;
			foundBuiltIn = isBuiltIn;
		}

		if (deviceId) {
			CFRelease(deviceId);
		}

		if (builtIn) {
			CFRelease(builtIn);
		}

		IOObjectRelease(service);
		if (foundBuiltIn) {
			break;
		}
	}

	IOObjectRelease(iterator);

	return multitouchRegistryId != 0 && multitouchDeviceId != 0;
}

static bool loadTouchApi(void) {
	void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	void *skylight = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW);
	if (!iokit || !skylight) {
		return false;
	}

	touchApi.createDigitizer = dlsym(iokit, "IOHIDEventCreateDigitizerEvent");
	touchApi.createFinger = dlsym(iokit, "IOHIDEventCreateDigitizerFingerEvent");
	touchApi.createVendorDefined = dlsym(iokit, "IOHIDEventCreateVendorDefinedEvent");
	touchApi.setSenderID = dlsym(iokit, "IOHIDEventSetSenderID");
	touchApi.setIntegerValue = dlsym(iokit, "IOHIDEventSetIntegerValue");
	touchApi.appendEvent = dlsym(iokit, "IOHIDEventAppendEvent");
	touchApi.attachToEvent = dlsym(skylight, "SLEventSetIOHIDEvent");
	bool loaded = touchApi.createDigitizer && touchApi.createFinger && touchApi.createVendorDefined && touchApi.setSenderID && touchApi.setIntegerValue && touchApi.appendEvent && touchApi.attachToEvent;

	return loaded && findMultitouchDevice();
}

static CFNumberRef createNumber(int value) {
	return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
			i++;
			double requested = strtod(argv[i], NULL);
			if (requested > 0) {
				speedFactor = requested;
			}
		} else if (strcmp(argv[i], "--verbose") == 0) {
			verbose = true;
		}
	}

	const void *promptKeys[] = {kAXTrustedCheckOptionPrompt};
	const void *promptValues[] = {kCFBooleanTrue};
	CFDictionaryRef promptOptions = CFDictionaryCreate(kCFAllocatorDefault, promptKeys, promptValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	accessibilityTrusted = AXIsProcessTrustedWithOptions(promptOptions);
	CFRelease(promptOptions);
	inputMonitoringGranted = checkInputMonitoring();
	if (!inputMonitoringGranted) {
		IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
	}

	dockSwipesAvailable = loadDockSwipeApi();
	touchEventsAvailable = loadTouchApi();
	eventSource = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
	CGEventSourceSetLocalEventsSuppressionInterval(eventSource, 0);

	IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	CFNumberRef vendor = createNumber(VENDOR_ID);
	CFNumberRef product = createNumber(PRODUCT_ID);
	CFNumberRef usagePage = createNumber(kHIDPage_GenericDesktop);
	CFNumberRef usage = createNumber(kHIDUsage_GD_Mouse);
	const void *matchKeys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey), CFSTR(kIOHIDPrimaryUsagePageKey), CFSTR(kIOHIDPrimaryUsageKey)};
	const void *matchValues[] = {vendor, product, usagePage, usage};
	CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault, matchKeys, matchValues, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOHIDManagerSetDeviceMatching(manager, matching);
	IOHIDManagerRegisterDeviceMatchingCallback(manager, onDeviceMatched, NULL);
	IOHIDManagerRegisterDeviceRemovalCallback(manager, onDeviceRemoved, NULL);
	IOHIDManagerRegisterInputReportCallback(manager, onInputReport, NULL);
	IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	IOReturn openResult = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);

	CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + MODE_REFRESH_SECONDS, MODE_REFRESH_SECONDS, 0, 0, onRefreshTimer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

	logLine("started: speed %.2f, verbose: %s, accessibility: %s, input monitoring: %s, dock swipes: %s, touches: %s, HID manager open: 0x%08x", speedFactor, yesNo(verbose), yesNo(accessibilityTrusted), yesNo(inputMonitoringGranted), yesNo(dockSwipesAvailable), yesNo(touchEventsAvailable), openResult);

	CFRunLoopRun();

	return 0;
}
