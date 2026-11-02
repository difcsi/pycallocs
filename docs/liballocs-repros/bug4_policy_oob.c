/* Bug 4 (docs/LIBALLOCS_BUGS.md): __liballocs_register_gc_policy
 * off-by-one.
 *
 * src/lifetime_policies.c:
 *     static struct lifetime_policy __lifetime_policies[LIFETIME_POLICIES];
 *     int id = __last_free_lifetime_policy_id++;   // starts at 1
 *     if (id > LIFETIME_POLICIES) return -1;       // admits id == 4
 *     __lifetime_policies[id] = ...;               // [4] is out of bounds
 *
 * With LIFETIME_POLICIES == 4 and id 0 reserved for the manual-
 * deallocation policy, the valid dynamic ids are 1..3. The fourth
 * registration should fail with -1; instead it returns 4 after
 * writing one element past the array. Fix: `if (id >= LIFETIME_POLICIES)`.
 */

#include <stdio.h>

typedef void (*gc_callback)(const void *target, const void **from);
extern int __liballocs_register_gc_policy(gc_callback addref,
                                          gc_callback delref);

static void addref(const void *target, const void **from)
{ (void)target; (void)from; }
static void delref(const void *target, const void **from)
{ (void)target; (void)from; }

int main(void)
{
    int i, bug = 0;
    for (i = 1; i <= 5; i++) {
        int id = __liballocs_register_gc_policy(addref, delref);
        printf("registration %d -> id %d%s\n", i, id,
               (id == 4) ? "   <-- BUG: array has slots 0..3;"
                           " this registration wrote out of bounds"
                           " (should have returned -1)"
                         : "");
        if (id == 4) bug = 1;
    }
    return bug ? 0 : 1;  /* exit 0 = bug reproduced */
}
