/* nn::oe: remaining query/command entry points, natively (generated).
 *
 * The run-environment queries and commands a game calls during init that hle_env.c does
 * not already define. Most only report success; a few fill a fixed-size output. Values
 * are the defaults of a handheld, Japanese console. Regenerate with the helper that
 * produced this file if the set of unimplemented nn::oe exports changes. */
#include "host.h"

HLE(_ZN2nn2oe15RequestToRebootEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe16InitializeAppletEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe18GetPerformanceModeEv) { c->x[0] = 0; }        /* PerformanceMode_Normal */
HLE(_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 0x10); c->x[0] = 0;
}
HLE(_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE) {
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 16); c->x[0] = 0;
}
HLE(_ZN2nn2oe17RequestToShutdownEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe18ReportUserIsActiveEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe20GetDefaultThemeColorEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe20NotifyRunningForTestEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe21EndBlockingHomeButtonEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe21GetExpectedThemeColorEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe21SetExpectedThemeColorENS0_14ThemeColorTypeE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe21TryPopLaunchParameterEPmPvm) {
    if (c->x[0]) *(u64 *)(uintptr_t)c->x[0] = 0; c->x[0] = 0;
}
HLE(_ZN2nn2oe22GetNotificationMessageEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe22PauseGamePlayRecordingEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe22PopNotificationMessageEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe23BeginBlockingHomeButtonEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe23EnableGamePlayRecordingEPvm) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe23EndMediaPlaybackSectionEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe24RestartGamePlayRecordingEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe25BeginMediaPlaybackSectionEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_) {
    st32(c->x[0], 1280); st32(c->x[1], 720); c->x[0] = 0;
}
HLE(_ZN2nn2oe27GetNotificationMessageEventEv) {
    /* returns an os::SystemEventType* the caller initialises in place: hand back a real
     * (zeroed) guest buffer, not 0. */
    static const char zero[32] = {0};
    c->x[0] = rt_guest_copy(zero, sizeof zero);
}
HLE(_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe27SetPerformanceConfigurationEi) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe28GetLaunchStorageInfoForDebugEPNS0_25LaunchStorageInfoForDebugE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe28IsGamePlayRecordingSupportedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe28SetFocusHandlingModeForDebugEbbbb) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe28SetResumeNotificationEnabledEb) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe30InvalidateNnOeInitializeAppletEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe30SetExpectedThemeColorForSystemENS0_14ThemeColorTypeE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe33EndBlockingHomeButtonShortPressedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe35BeginBlockingHomeButtonShortPressedENS_8TimeSpanE) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe35BeginBlockingHomeButtonShortPressedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe36EndHandlingCaptureButtonShortPressedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe38BeginHandlingCaptureButtonShortPressedEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE) {
    /* (SystemEventType* out) -> result: zero it so a later initialise-in-place has real memory. */
    if (c->x[0]) memset((void *)(uintptr_t)c->x[0], 0, 32);
    c->x[0] = 0;
}
HLE(_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb) {
    c->x[0] = 0;
}
HLE(_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb) {
    c->x[0] = 0;
}
