/* Start-up of the SDK's service-backed subsystems. */
#include "host.h"

/* Called once by nn::init::Start. The original starts diagnostics logging, the operating
 * environment, add-on content, the file system and parental controls - each by opening a
 * session to a console system service - plus the OS layer's virtual-address memory.
 * Here those subsystems are native and need no session, so only the OS step remains. */
HLE(nninitInitializeSdkModule) {
    rt_call(c, rt_symbol("_ZN2nn2os30InitializeVirtualAddressMemoryEv"));
}
