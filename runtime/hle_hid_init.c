/* nn::hid initialisation entry points, natively.
 *
 * hid state queries (GetNpadState, style sets, vibration, touch, six-axis) are implemented in
 * hle_hid.c and reached through the recompiler's native forwarding, so they need no console
 * side. But a game's nnMain first calls the SDK's nn::hid::Initialize* functions, whose internal
 * SharedMemoryAccessor / SharedMemoryHolder machinery expects a fully brought-up hid service
 * (shared memory, applet-resource user id, server session). For an engine that initialises hid
 * this way (Xenoblade 2), that machinery dereferences a global accessor a standalone runtime
 * never fills, and faults.
 *
 * Since the state is supplied natively already, these initialisers only need to report success.
 * Returning success here skips the fragile internal path and leaves the query functions in
 * hle_hid.c as the live source of input. Both the public wrappers and the ::detail:: bodies are
 * replaced, so whichever level a caller reaches, it is safe.
 */
#include "host.h"

/* public wrappers (InitializeNpad, InitializeTouchScreen and InitializeVibrationDevice are
 * already defined in hle_hid.c; do not redefine them here) */
HLE(_ZN2nn3hid15InitializeMouseEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid18InitializeKeyboardEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid17InitializeGestureEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid18InitializeDebugPadEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid30InitializeConsoleSixAxisSensorEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid14InitializeXpadERKNS0_11BasicXpadIdE) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid14InitializeXpadERKNS0_9JoyXpadIdE) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid5debug24InitializeFirmwareUpdateEv) { (void)c; c->x[0] = 0; }

/* ::detail:: bodies (reached directly along some SDK paths) */
HLE(_ZN2nn3hid6detail14InitializeNpadEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail15InitializeMouseEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail18InitializeKeyboardEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail21InitializeTouchScreenEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail17InitializeGestureEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail18InitializeDebugPadEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail30InitializeConsoleSixAxisSensorEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail14InitializeXpadERKNS0_11BasicXpadIdE) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail14InitializeXpadERKNS0_9JoyXpadIdE) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail20InitializeHomeButtonEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail21InitializeSleepButtonEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail23InitializeCaptureButtonEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail23InitializeInputDetectorEv) { (void)c; c->x[0] = 0; }
HLE(_ZN2nn3hid6detail24InitializeFirmwareUpdateEv) { (void)c; c->x[0] = 0; }
