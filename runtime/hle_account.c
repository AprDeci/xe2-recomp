/* nn::account additions.
 *
 * hle_env.c already implements the account surface (Initialize, GetUserCount, ListAllUsers,
 * ListOpenUsers, GetUserId, CloseUser, the network-service-account stubs, ...). Xenoblade 2's
 * nnMain also calls OpenPreselectedUser, which hle_env.c does not define; add only that here so
 * the account surface is complete. Every other account symbol the modules import is already in
 * hle_env.c, so it is not redefined here (a duplicate would fail to link).
 */
#include "host.h"

/* OpenPreselectedUser(UserHandle* out) -> result. hle_env.c's USER id names one local user. */
HLE(_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE) {
    if (c->x[0]) {
        st64(c->x[0], 0x0000000000000000ull);
        st64(c->x[0] + 8, 0x0000000100000000ull);
    }
    c->x[0] = 0;
}
