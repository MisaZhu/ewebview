/* Host stubs for target-only symbols (libc asm aliases on EwokOS). */
#include <setjmp.h>

int gumbo_setjmp(jmp_buf env);
void gumbo_longjmp(jmp_buf env, int val);
int ewok_ptr_in_heap(const void* p);

int gumbo_setjmp(jmp_buf env)
{
    return setjmp(env);
}

void gumbo_longjmp(jmp_buf env, int val)
{
    longjmp(env, val);
}

int ewok_ptr_in_heap(const void* p)
{
    (void)p;
    return 1;
}
