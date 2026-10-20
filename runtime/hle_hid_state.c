/* nn::hid input-state readers, natively (generated).
 *
 * The single-state Npad queries live in hle_hid.c. A game also reads the *plural* and
 * template forms (touch screen, six-axis, mouse, keyboard, gesture) whose bodies walk the
 * console's hid shared-memory ring buffers. Those buffers are not populated here, so the
 * readers would fault; instead they report 'no new state': the count is 0 / the first bytes
 * of the caller's state buffer are cleared (sampling time and count live in the first 0x10
 * bytes), which makes the game see an idle controller and keep going.
 */
#include "host.h"

HLE(_ZN2nn3hid12GetXpadStateEPNS0_14BasicXpadStateERKNS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid13GetXpadStatesEPNS0_14BasicXpadStateEiRKNS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid17GetDebugPadStatesEPNS0_13DebugPadStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid17GetKeyboardStatesEPNS0_13KeyboardStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetJoyXpadLeftStateEPNS0_12JoyXpadStateERKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm10EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm11EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm12EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm13EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm14EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm15EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm2EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm3EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm4EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm5EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm6EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm7EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm8EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid19GetTouchScreenStateILm9EEEvPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetJoyXpadLeftStatesEPNS0_12JoyXpadStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetJoyXpadRightStateEPNS0_12JoyXpadStateERKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm10EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm11EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm12EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm13EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm14EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm15EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm2EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm3EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm4EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm5EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm6EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm7EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm8EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm9EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid21GetJoyXpadRightStatesEPNS0_12JoyXpadStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid3tmp34GetConsoleSixAxisSensorCountStatesEPNS1_23SixAxisSensorCountStateEiRKNS0_26ConsoleSixAxisSensorHandleE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug21SetXpadAutoPilotStateERKNS0_11BasicXpadIdERKNS1_23BasicXpadAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug22SetMouseAutoPilotStateERKNS1_19MouseAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug23UnsetXpadAutoPilotStateERKNS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug24UnsetMouseAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug25SetDebugPadAutoPilotStateERKNS1_22DebugPadAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug25SetKeyboardAutoPilotStateERKNS1_22KeyboardAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug27SetHomeButtonAutoPilotStateERKNS1_24HomeButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug27UnsetDebugPadAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug27UnsetKeyboardAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetSleepButtonAutoPilotStateERKNS1_25SleepButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm10EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm11EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm12EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm13EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm14EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm15EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm16EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm1EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm2EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm3EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm4EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm5EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm6EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm7EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm8EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug28SetTouchScreenAutoPilotStateILm9EEEvRKNS1_25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug29UnsetHomeButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug30SetCaptureButtonAutoPilotStateERKNS1_27CaptureButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug30UnsetSleepButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug30UnsetTouchScreenAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid5debug32UnsetCaptureButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail10GetXpadIdsEPiPNS0_11BasicXpadIdEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail10GetXpadIdsEPiPNS0_9JoyXpadIdEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail12GetXpadStateEPNS0_14BasicXpadStateERKNS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetMouseStateEPNS0_10MouseStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadFullKeyStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyDualStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_16NpadJoyLeftStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadHandheldStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_17NpadJoyRightStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_6system15NpadSystemStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_6system18NpadSystemExtStateEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail13GetXpadStatesEPiPNS0_14BasicXpadStateEiRKNS0_11BasicXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail14GetMouseStatesEPiPNS0_10MouseStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail14IsNfcActivatedEPbRj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail14IsUsbConnectedEPbNS0_6system11UniquePadIdE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail14ListUniquePadsEPiPNS0_6system11UniquePadIdEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail15GetNpadsWithNfcEPiPji) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail16GetDebugPadStateEPNS0_13DebugPadStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail16GetGestureStatesEPiPNS0_12GestureStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail16GetIrSensorStateEPNS0_6system13IrSensorStateERj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail16GetKeyboardStateEPNS0_13KeyboardStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail17GetDebugPadStatesEPiPNS0_13DebugPadStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail17GetKeyboardStatesEPiPNS0_13KeyboardStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetAnalogStickStateEPNS0_16AnalogStickStateENS0_6system11UniquePadIdENS4_19AnalogStickPositionE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetHomeButtonStatesEPiPNS0_6system15HomeButtonStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetInputSourceStateEPNS0_6system16InputSourceStateERKNS_4util10BitFlagSetILi32ENS2_13InputSourceIdEEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetJoyXpadLeftStateEPNS0_12JoyXpadStateERKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm10EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm11EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm12EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm13EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm14EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm15EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm16EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm1EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm2EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm3EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm4EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm5EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm6EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm7EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm8EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail19GetTouchScreenStateILm9EEENS_6ResultEPNS0_16TouchScreenStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetJoyXpadLeftStatesEPiPNS0_12JoyXpadStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetJoyXpadRightStateEPNS0_12JoyXpadStateERKNS0_9JoyXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetRegisteredDevicesEPiPNS0_6system16RegisteredDeviceEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetSleepButtonStatesEPiPNS0_6system16SleepButtonStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm10EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm11EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm12EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm13EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm14EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm15EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm16EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm1EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm2EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm3EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm4EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm5EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm6EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm7EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm8EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20GetTouchScreenStatesILm9EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20IsAbxyButtonOrientedEPbRKj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20IsSlSrButtonOrientedEPbRKj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail20IsVibrationPermittedEPb) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail21GetJoyXpadRightStatesEPiPNS0_12JoyXpadStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail21GetUniquePadsFromNpadEPiPNS0_6system11UniquePadIdEiRKj) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail21IsSixAxisSensorAtRestEPbRKNS0_19SixAxisSensorHandleE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail21SetXpadAutoPilotStateENS0_11BasicXpadIdERKNS0_5debug23BasicXpadAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail22GetCaptureButtonStatesEPiPNS0_6system18CaptureButtonStateEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_11BasicXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail22SetMouseAutoPilotStateERKNS0_5debug19MouseAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail23UnsetXpadAutoPilotStateENS0_11BasicXpadIdE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail24ListSixAxisSensorHandlesEPiPNS0_6system25UniqueSixAxisSensorHandleENS3_11UniquePadIdEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail24UnsetMouseAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail25SetDebugPadAutoPilotStateERKNS0_5debug22DebugPadAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail25SetKeyboardAutoPilotStateERKNS0_5debug22KeyboardAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail26GetSixAxisSensorLeftStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail26GetSixAxisSensorLeftStatesEPiPNS0_18SixAxisSensorStateEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail26IsAnalogStickButtonPressedEPbNS0_6system11UniquePadIdENS3_19AnalogStickPositionE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail26IsUnsupportedButtonPressedEPbRKjRKNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail27GetSixAxisSensorRightStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_9JoyXpadIdE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail27GetSixAxisSensorRightStatesEPiPNS0_18SixAxisSensorStateEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail27SetHomeButtonAutoPilotStateERKNS0_5debug24HomeButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail27UnsetDebugPadAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail27UnsetKeyboardAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28IsAnalogStickInCircumferenceEPbNS0_6system11UniquePadIdENS3_19AnalogStickPositionE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28IsSixAxisSensorFusionEnabledEPbRKNS0_19SixAxisSensorHandleE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetSleepButtonAutoPilotStateERKNS0_5debug25SleepButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm10EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm11EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm12EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm13EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm14EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm15EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm16EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm1EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm2EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm3EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm4EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm5EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm6EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm7EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm8EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail28SetTouchScreenAutoPilotStateILm9EEENS_6ResultERKNS0_5debug25TouchScreenAutoPilotStateIXT_EEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail29GetPlayReportControllerUsagesEPiPNS0_6system25PlayReportControllerUsageEi) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail29IsUsbFullKeyControllerEnabledEPb) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail29UnsetHomeButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail30IsAnalogStickInReleasePositionEPbNS0_6system11UniquePadIdENS3_19AnalogStickPositionE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail30SetCaptureButtonAutoPilotStateERKNS0_5debug27CaptureButtonAutoPilotStateE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail30UnsetSleepButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail30UnsetTouchScreenAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail32UnsetCaptureButtonAutoPilotStateEv) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail34GetConsoleSixAxisSensorCountStatesEPiPNS0_3tmp23SixAxisSensorCountStateEiNS0_26ConsoleSixAxisSensorHandleE) {
    if (c->x[0]) st32(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail39IsSixAxisSensorUserCalibrationSupportedEPbNS0_6system25UniqueSixAxisSensorHandleE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail43IsUsbFullKeyControllerEnabledForApplicationEPb) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail44IsUnintendedHomeButtonInputProtectionEnabledEPbRKj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail45IsUsbFullKeyControllerConnectedForApplicationEPbRKj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6detail8RingLifoINS0_13DebugPadStateELi16ENS1_13AtomicStorageIS3_EEED2Ev) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system12GetNpadStateEPNS1_15NpadSystemStateERKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system12GetNpadStateEPNS1_18NpadSystemExtStateERKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system13GetNpadStatesEPNS1_15NpadSystemStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system13GetNpadStatesEPNS1_18NpadSystemExtStateEiRKj) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system14IsNpadChargingEPbS2_RKj) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system19GetAnalogStickStateEPNS0_16AnalogStickStateENS1_11UniquePadIdENS1_19AnalogStickPositionE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system19GetHomeButtonStatesEPNS1_15HomeButtonStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system19GetInputSourceStateEPNS1_16InputSourceStateERKNS_4util10BitFlagSetILi32ENS1_13InputSourceIdEEE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system20GetSleepButtonStatesEPNS1_16SleepButtonStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system22GetCaptureButtonStatesEPNS1_18CaptureButtonStateEi) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system22GetFirmwareUpdateStateEPNS1_19FirmwareUpdateStateERKNS1_26FirmwareUpdateDeviceHandleE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system25IsFirmwareUpdateAvailableEPbNS1_11UniquePadIdE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system6detail22GetFirmwareUpdateStateEPNS1_19FirmwareUpdateStateERKNS1_26FirmwareUpdateDeviceHandleE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn3hid6system6detail25IsFirmwareUpdateAvailableEPbNS1_11UniquePadIdE) {
    if (c->x[0]) st8(c->x[0], 0); c->x[0] = 0;
}
